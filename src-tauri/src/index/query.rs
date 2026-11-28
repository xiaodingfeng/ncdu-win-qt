//! Query kernel: parses the Everything-style subset (AND / OR / NOT, wildcards,
//! `ext:` / `path:` / `size:` / `dm:`) and scans the compact index in parallel.
//!
//! Matching is compiled once per request (memmem needle, memchr2 first-byte
//! jump, compiled wildcard, numeric extension id) and scans in 4096-entry
//! chunks that keep only their local Top-K, so memory stays bounded even when
//! millions of entries match.

use std::cell::RefCell;
use std::cmp::Ordering;
use std::sync::atomic::{AtomicBool, Ordering as AtomicOrdering};

use memchr::{memchr2, memmem};
use rayon::prelude::*;
use serde::Serialize;

use super::builder::now_secs;
use super::model::{fold_contains, Index};

#[derive(Debug, Clone)]
pub struct SearchRequest {
    pub(crate) query: String,
    pub(crate) scope: Option<String>,
    pub(crate) sort: String,
    pub(crate) asc: bool,
    pub(crate) offset: usize,
    pub(crate) limit: usize,
    pub(crate) case_sensitive: bool,
}

impl Default for SearchRequest {
    fn default() -> Self {
        Self {
            query: String::new(),
            scope: None,
            sort: "size".to_string(),
            asc: false,
            offset: 0,
            limit: 200,
            case_sensitive: false,
        }
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SearchItem {
    pub(crate) path: String,
    pub(crate) name: String,
    pub(crate) size: u64,
    pub(crate) mtime: i64,
    pub(crate) is_dir: bool,
    pub(crate) hidden: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SearchResponse {
    pub(crate) items: Vec<SearchItem>,
    pub(crate) total: usize,
    pub(crate) took_ms: u64,
    pub(crate) version: u64,
    pub(crate) has_more: bool,
    pub(crate) error: Option<String>,
}

thread_local! {
    /// Reused by scope and `path:` filters so no per-entry String is allocated.
    static PATH_BUF: RefCell<String> = const { RefCell::new(String::new()) };
}

/// A pre-compiled substring / wildcard matcher.
enum Needle {
    Memmem(memmem::Finder<'static>),
    AsciiCi {
        lower: Vec<u8>,
        first: (u8, u8),
    },
    /// Case-insensitive needle with non-ASCII bytes: ASCII folding only.
    Fold(Vec<u8>),
    Wildcard(Wildcard),
}

struct Wildcard {
    pattern: String,
    chars: Vec<char>,
    case: bool,
}

impl Wildcard {
    fn matches(&self, text: &str) -> bool {
        if text.is_ascii() && self.pattern.is_ascii() {
            wildcard_bytes(text.as_bytes(), self.pattern.as_bytes(), self.case)
        } else {
            wildcard_chars(text, &self.chars, self.case)
        }
    }
}

impl Needle {
    fn compile(text: &str, case: bool) -> Self {
        if text.contains('*') || text.contains('?') {
            let pattern = if case {
                text.to_string()
            } else {
                text.to_ascii_lowercase()
            };
            let chars = pattern.chars().collect();
            return Needle::Wildcard(Wildcard {
                pattern,
                chars,
                case,
            });
        }
        if case {
            return Needle::Memmem(memmem::Finder::new(text.as_bytes()).into_owned());
        }
        let lower = text.to_ascii_lowercase();
        if lower.is_ascii() {
            let bytes = lower.into_bytes();
            let first = bytes
                .first()
                .map(|value| {
                    if value.is_ascii_alphabetic() {
                        (value.to_ascii_lowercase(), value.to_ascii_uppercase())
                    } else {
                        (*value, *value)
                    }
                })
                .unwrap_or((0, 0));
            Needle::AsciiCi { lower: bytes, first }
        } else {
            Needle::Fold(lower.into_bytes())
        }
    }

    fn matches(&self, text: &str) -> bool {
        match self {
            Needle::Memmem(finder) => finder.find(text.as_bytes()).is_some(),
            Needle::AsciiCi { lower, first } => {
                ascii_ci_contains(text.as_bytes(), lower, *first)
            }
            Needle::Fold(lower) => {
                let needle = std::str::from_utf8(lower).unwrap_or("");
                fold_contains(text, needle)
            }
            Needle::Wildcard(pattern) => pattern.matches(text),
        }
    }
}

enum Matcher {
    Name(Needle),
    Path(Needle),
    Ext(Option<u16>),
    SizeAtLeast(u64),
    SizeAtMost(u64),
    MtimeAfter(i64),
}

struct Term {
    negated: bool,
    matcher: Matcher,
}

#[cfg(test)]
pub(crate) fn search(index: &Index, request: &SearchRequest) -> SearchResponse {
    search_with_cancel(index, request, None)
}

pub(crate) fn search_with_cancel(
    index: &Index,
    request: &SearchRequest,
    cancel: Option<&AtomicBool>,
) -> SearchResponse {
    let started = std::time::Instant::now();
    let limit = request.limit.clamp(1, 1000);
    let (groups, parse_error) = parse_query(index, &request.query, request.case_sensitive);
    let scope = request
        .scope
        .as_ref()
        .map(|value| normalize_scope(value))
        .filter(|value| !value.is_empty());
    let empty = |error: Option<String>, total: usize| SearchResponse {
        items: Vec::new(),
        total,
        took_ms: started.elapsed().as_millis() as u64,
        version: index.version,
        has_more: false,
        error,
    };
    if groups.is_empty() && scope.is_none() {
        return empty(parse_error, 0);
    }

    let is_cancelled = || cancel.map(|flag| flag.load(AtomicOrdering::Relaxed)).unwrap_or(false);
    if is_cancelled() {
        return empty(None, 0);
    }

    let volume_root = format!("{}:\\", index.volume).to_ascii_lowercase();
    let skip_scope = scope.as_ref().map(|value| *value == volume_root).unwrap_or(false);
    let keep = |idx: u32| -> bool {
        if idx == 0 || !index.is_live(idx) || index.name(idx).is_empty() {
            return false;
        }
        if let Some(scope) = &scope {
            if !skip_scope {
                let matched = PATH_BUF.with(|buffer| {
                    let mut buffer = buffer.borrow_mut();
                    index.path_into(idx, &mut buffer);
                    fold_contains(&buffer, scope)
                });
                if !matched {
                    return false;
                }
            }
        }
        matches_entry(index, idx, &groups)
    };

    let need = (request.offset + limit).clamp(1, 1000);
    let asc = request.asc;
    let compare = |left: &u32, right: &u32| compare_slot(index, *left, *right, &request.sort, asc);

    const CHUNK: usize = 4096;
    let len = index.len();
    let chunk_count = len.div_ceil(CHUNK);
    let scan_chunk = |chunk: usize| -> Option<(usize, Vec<u32>)> {
        let start = chunk * CHUNK;
        let end = (start + CHUNK).min(len);
        let mut count = 0usize;
        let mut hits: Vec<u32> = Vec::new();
        for position in start..end {
            if (position - start) % 1024 == 0 && is_cancelled() {
                return None;
            }
            let idx = position as u32;
            if keep(idx) {
                count += 1;
                hits.push(idx);
            }
        }
        if hits.len() > need {
            hits.select_nth_unstable_by(need, &compare);
            hits.truncate(need);
        }
        Some((count, hits))
    };

    let partials: Vec<Option<(usize, Vec<u32>)>> = match super::store::index_pool() {
        Some(pool) => pool.install(|| (0..chunk_count).into_par_iter().map(scan_chunk).collect()),
        None => (0..chunk_count).into_par_iter().map(scan_chunk).collect(),
    };

    let mut total = 0usize;
    let mut merged: Vec<u32> = Vec::new();
    for partial in partials {
        let Some((count, hits)) = partial else {
            // A newer query superseded this one; return immediately.
            return empty(None, 0);
        };
        total += count;
        merged.extend(hits);
    }
    if merged.len() > need {
        merged.select_nth_unstable_by(need, &compare);
        merged.truncate(need);
    }
    merged.sort_by(&compare);

    let offset = request.offset.min(merged.len());
    let end = (offset + limit).min(merged.len());
    let items: Vec<SearchItem> = merged[offset..end]
        .iter()
        .map(|idx| SearchItem {
            path: index.path_of(*idx),
            name: index.name(*idx).to_string(),
            size: index.sizes[*idx as usize],
            mtime: index.mtimes[*idx as usize] as i64,
            is_dir: index.is_dir(*idx),
            hidden: index.is_hidden(*idx),
        })
        .collect();
    let reached = offset + items.len();
    SearchResponse {
        has_more: reached < total,
        items,
        total,
        took_ms: started.elapsed().as_millis() as u64,
        version: index.version,
        error: parse_error,
    }
}

/// Sorts an already-collected page of hits. Used when several volumes are
/// merged: each volume returns its own top slice, then the union is re-sorted
/// with the same comparison rules.
pub(crate) fn sort_items(items: &mut [SearchItem], sort: &str, asc: bool) {
    match sort {
        "name" => items.sort_by(|left, right| compare_text(&left.name, &right.name, asc)),
        "path" => items.sort_by(|left, right| compare_text(&left.path, &right.path, asc)),
        "mtime" => items.sort_by(|left, right| order(left.mtime.cmp(&right.mtime), asc)),
        _ => items.sort_by(|left, right| order(left.size.cmp(&right.size), asc)),
    }
}

fn order(value: Ordering, asc: bool) -> Ordering {
    if asc {
        value
    } else {
        value.reverse()
    }
}

fn compare_slot(index: &Index, left: u32, right: u32, sort: &str, asc: bool) -> Ordering {
    let comparison = match sort {
        "name" => compare_text(index.name(left), index.name(right), true),
        "path" => compare_paths(index, left, right),
        "mtime" => index.mtimes[left as usize].cmp(&index.mtimes[right as usize]),
        _ => index.sizes[left as usize].cmp(&index.sizes[right as usize]),
    };
    order(comparison, asc)
}

/// Compares two paths component by component without building path strings.
fn compare_paths(index: &Index, left: u32, right: u32) -> Ordering {
    let mut left_chain = [u32::MAX; 96];
    let mut right_chain = [u32::MAX; 96];
    let (left_len, left_overflow) = fill_chain(index, left, &mut left_chain);
    let (right_len, right_overflow) = fill_chain(index, right, &mut right_chain);
    if left_overflow || right_overflow {
        return compare_text(&index.path_of(left), &index.path_of(right), true);
    }
    for step in 0..left_len.min(right_len) {
        let left_slot = left_chain[left_len - 1 - step];
        let right_slot = right_chain[right_len - 1 - step];
        let comparison = compare_text(index.name(left_slot), index.name(right_slot), true);
        if comparison != Ordering::Equal {
            return comparison;
        }
    }
    left_len.cmp(&right_len)
}

fn fill_chain(index: &Index, idx: u32, chain: &mut [u32; 96]) -> (usize, bool) {
    let mut len = 0usize;
    let mut current = idx;
    let mut depth = 0u32;
    loop {
        if index.name(current).is_empty() {
            break;
        }
        if len >= chain.len() {
            return (len, true);
        }
        chain[len] = current;
        len += 1;
        let Some(parent) = index.parents.get(current as usize).copied() else {
            break;
        };
        if parent == u32::MAX || parent == current {
            break;
        }
        current = parent;
        depth += 1;
        if depth > 512 {
            break;
        }
    }
    (len, false)
}

fn compare_text(left: &str, right: &str, asc: bool) -> Ordering {
    if left.is_ascii() && right.is_ascii() {
        return order(compare_ascii(left.as_bytes(), right.as_bytes()), asc);
    }
    let mut left_chars = left.chars();
    let mut right_chars = right.chars();
    loop {
        match (left_chars.next(), right_chars.next()) {
            (None, None) => return Ordering::Equal,
            (None, Some(_)) => return order(Ordering::Less, asc),
            (Some(_), None) => return order(Ordering::Greater, asc),
            (Some(left_char), Some(right_char)) => {
                let comparison = super::model::fold_char(left_char)
                    .cmp(&super::model::fold_char(right_char));
                if comparison != Ordering::Equal {
                    return order(comparison, asc);
                }
            }
        }
    }
}

fn compare_ascii(left: &[u8], right: &[u8]) -> Ordering {
    let mut position = 0usize;
    loop {
        match (left.get(position), right.get(position)) {
            (None, None) => return Ordering::Equal,
            (None, Some(_)) => return Ordering::Less,
            (Some(_), None) => return Ordering::Greater,
            (Some(left_byte), Some(right_byte)) => {
                let comparison = left_byte
                    .to_ascii_lowercase()
                    .cmp(&right_byte.to_ascii_lowercase());
                if comparison != Ordering::Equal {
                    return comparison;
                }
            }
        }
        position += 1;
    }
}

fn matches_entry(index: &Index, idx: u32, groups: &[Vec<Term>]) -> bool {
    if groups.is_empty() {
        return true;
    }
    groups.iter().any(|terms| {
        terms
            .iter()
            .all(|term| term.negated != term_hits(index, idx, term))
    })
}

fn term_hits(index: &Index, idx: u32, term: &Term) -> bool {
    match &term.matcher {
        Matcher::Name(needle) => needle.matches(index.name(idx)),
        Matcher::Path(needle) => PATH_BUF.with(|buffer| {
            let mut buffer = buffer.borrow_mut();
            index.path_into(idx, &mut buffer);
            needle.matches(&buffer)
        }),
        Matcher::Ext(Some(id)) => index.ext_ids.get(idx as usize) == Some(id),
        Matcher::Ext(None) => false,
        Matcher::SizeAtLeast(min) => index.sizes[idx as usize] >= *min,
        Matcher::SizeAtMost(max) => index.sizes[idx as usize] <= *max,
        Matcher::MtimeAfter(after) => index.mtimes[idx as usize] as i64 >= *after,
    }
}

/// Case-insensitive ASCII substring search with a SIMD first-byte jump.
fn ascii_ci_contains(haystack: &[u8], lower: &[u8], first: (u8, u8)) -> bool {
    if lower.is_empty() {
        return true;
    }
    if lower.len() > haystack.len() {
        return false;
    }
    let last = haystack.len() - lower.len();
    let mut start = 0usize;
    while start <= last {
        let window = &haystack[start..];
        let Some(found) = memchr2(first.0, first.1, &window[..=last - start]) else {
            return false;
        };
        let position = start + found;
        if haystack[position..position + lower.len()]
            .iter()
            .zip(lower)
            .all(|(left, right)| left.to_ascii_lowercase() == *right)
        {
            return true;
        }
        start = position + 1;
    }
    false
}

fn wildcard_bytes(text: &[u8], pattern: &[u8], case: bool) -> bool {
    let (mut text_pos, mut pattern_pos) = (0usize, 0usize);
    let (mut star, mut resume) = (None, 0usize);
    let equals = |left: u8, right: u8| {
        if case {
            left == right
        } else {
            left.to_ascii_lowercase() == right.to_ascii_lowercase()
        }
    };
    while text_pos < text.len() {
        if pattern_pos < pattern.len()
            && (pattern[pattern_pos] == b'?' || equals(pattern[pattern_pos], text[text_pos]))
        {
            text_pos += 1;
            pattern_pos += 1;
        } else if pattern_pos < pattern.len() && pattern[pattern_pos] == b'*' {
            star = Some(pattern_pos);
            pattern_pos += 1;
            resume = text_pos;
        } else if let Some(star_pos) = star {
            pattern_pos = star_pos + 1;
            resume += 1;
            text_pos = resume;
        } else {
            return false;
        }
    }
    while pattern_pos < pattern.len() && pattern[pattern_pos] == b'*' {
        pattern_pos += 1;
    }
    pattern_pos == pattern.len()
}

fn wildcard_chars(text: &str, pattern: &[char], case: bool) -> bool {
    let text: Vec<char> = text.chars().collect();
    let (mut text_pos, mut pattern_pos) = (0usize, 0usize);
    let (mut star, mut resume) = (None, 0usize);
    let equals = |left: char, right: char| {
        if case {
            left == right
        } else {
            super::model::fold_char(left) == super::model::fold_char(right)
        }
    };
    while text_pos < text.len() {
        if pattern_pos < pattern.len()
            && (pattern[pattern_pos] == '?' || equals(pattern[pattern_pos], text[text_pos]))
        {
            text_pos += 1;
            pattern_pos += 1;
        } else if pattern_pos < pattern.len() && pattern[pattern_pos] == '*' {
            star = Some(pattern_pos);
            pattern_pos += 1;
            resume = text_pos;
        } else if let Some(star_pos) = star {
            pattern_pos = star_pos + 1;
            resume += 1;
            text_pos = resume;
        } else {
            return false;
        }
    }
    while pattern_pos < pattern.len() && pattern[pattern_pos] == '*' {
        pattern_pos += 1;
    }
    pattern_pos == pattern.len()
}

fn normalize_scope(scope: &str) -> String {
    let text = scope.trim().replace('/', "\\");
    let trimmed = text.trim_end_matches('\\');
    if trimmed.len() == 2 && trimmed.as_bytes()[1] == b':' {
        format!("{}\\", trimmed.to_ascii_lowercase())
    } else {
        trimmed.to_ascii_lowercase()
    }
}

fn tokenize(query: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut quoted = false;
    for value in query.chars() {
        match value {
            '"' => {
                quoted = !quoted;
                if !quoted && !current.is_empty() {
                    tokens.push(std::mem::take(&mut current));
                }
            }
            '|' if !quoted => {
                if !current.is_empty() {
                    tokens.push(std::mem::take(&mut current));
                }
                tokens.push("|".to_string());
            }
            value if value.is_whitespace() && !quoted => {
                if !current.is_empty() {
                    tokens.push(std::mem::take(&mut current));
                }
            }
            value => current.push(value),
        }
    }
    if !current.is_empty() {
        tokens.push(current);
    }
    tokens
}

fn parse_query(index: &Index, query: &str, case: bool) -> (Vec<Vec<Term>>, Option<String>) {
    let mut groups: Vec<Vec<Term>> = vec![Vec::new()];
    let mut error: Option<String> = None;
    for token in tokenize(query) {
        if token == "|" {
            if groups.last().map(|group| !group.is_empty()).unwrap_or(false) {
                groups.push(Vec::new());
            }
            continue;
        }
        match parse_term(index, &token, case) {
            Ok(Some(term)) => {
                if let Some(group) = groups.last_mut() {
                    group.push(term);
                }
            }
            Ok(None) => {}
            Err(message) => {
                if error.is_none() {
                    error = Some(message);
                }
            }
        }
    }
    groups.retain(|group| !group.is_empty());
    (groups, error)
}

fn parse_term(index: &Index, token: &str, case: bool) -> Result<Option<Term>, String> {
    let (negated, text) = match token.strip_prefix('!') {
        Some(rest) => (true, rest.trim()),
        None => (false, token.trim()),
    };
    if text.is_empty() {
        return Ok(None);
    }
    let lower = text.to_ascii_lowercase();
    if let Some(value) = lower.strip_prefix("ext:") {
        let extension = value.trim().trim_start_matches('.');
        if extension.is_empty() {
            return Err("ext: needs an extension".to_string());
        }
        return Ok(Some(Term {
            negated,
            matcher: Matcher::Ext(index.ext_id(extension)),
        }));
    }
    if let Some(value) = lower.strip_prefix("path:") {
        let value = value.trim();
        if value.is_empty() {
            return Err("path: needs a fragment".to_string());
        }
        return Ok(Some(Term {
            negated,
            matcher: Matcher::Path(Needle::compile(value, case)),
        }));
    }
    if let Some(value) = lower.strip_prefix("size:") {
        let value = value.trim();
        let matcher = parse_size(value)
            .ok_or_else(|| format!("invalid size filter: size:{value}"))?;
        return Ok(Some(Term { negated, matcher }));
    }
    if let Some(value) = lower.strip_prefix("dm:") {
        let value = value.trim();
        let after =
            parse_age(value).ok_or_else(|| format!("invalid date filter: dm:{value}"))?;
        return Ok(Some(Term {
            negated,
            matcher: Matcher::MtimeAfter(after),
        }));
    }
    let needle_text = if case {
        text.to_string()
    } else {
        lower
    };
    // A pasted full path should match the rebuilt path, like Everything.
    let matcher = if needle_text.contains('\\') {
        Matcher::Path(Needle::compile(&needle_text, case))
    } else {
        Matcher::Name(Needle::compile(&needle_text, case))
    };
    Ok(Some(Term { negated, matcher }))
}

fn parse_size(value: &str) -> Option<Matcher> {
    let (comparator, rest) = if let Some(rest) = value.strip_prefix(">=") {
        (">=", rest)
    } else if let Some(rest) = value.strip_prefix("<=") {
        ("<=", rest)
    } else if let Some(rest) = value.strip_prefix('>') {
        (">", rest)
    } else if let Some(rest) = value.strip_prefix('<') {
        ("<", rest)
    } else if let Some(rest) = value.strip_prefix('=') {
        ("=", rest)
    } else {
        (">=", value)
    };
    let bytes = parse_bytes(rest)?;
    Some(match comparator {
        ">" => Matcher::SizeAtLeast(bytes.saturating_add(1)),
        "<" => Matcher::SizeAtMost(bytes.saturating_sub(1)),
        _ => Matcher::SizeAtLeast(bytes),
    })
}

fn parse_bytes(text: &str) -> Option<u64> {
    let text = text.trim().to_ascii_lowercase();
    let (number, multiplier) = if let Some(value) = text.strip_suffix("tb") {
        (value, 1024u64.pow(4))
    } else if let Some(value) = text.strip_suffix("gb") {
        (value, 1024u64.pow(3))
    } else if let Some(value) = text.strip_suffix("mb") {
        (value, 1024u64.pow(2))
    } else if let Some(value) = text.strip_suffix("kb") {
        (value, 1024u64)
    } else if let Some(value) = text.strip_suffix('b') {
        (value, 1u64)
    } else {
        (text.as_str(), 1u64)
    };
    let value: f64 = number.trim().parse().ok()?;
    if value < 0.0 {
        return None;
    }
    Some((value * multiplier as f64) as u64)
}

fn parse_age(text: &str) -> Option<i64> {
    let now = now_secs();
    match text {
        "today" => Some(now - now.rem_euclid(86_400)),
        "week" | "thisweek" => Some(now - 7 * 86_400),
        "month" | "thismonth" => Some(now - 30 * 86_400),
        "year" | "thisyear" => Some(now - 365 * 86_400),
        value => {
            let days = value.strip_suffix('d')?;
            let days: i64 = days.parse().ok()?;
            Some(now - days * 86_400)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Index {
        let mut index = Index::new("C");
        let docs = index.push_entry(10, 0, "docs", 0x10, 0, 0);
        let notes = index.push_entry(11, docs, "notes.txt", 0, 100, 1_700_000_000);
        let report = index.push_entry(12, docs, "report-final.pdf", 0, 5_000, 1_700_000_100);
        let log = index.push_entry(13, 0, "setup.LOG", 0, 1_000_000, 1_690_000_000);
        index.set_parent(docs, 0);
        index.set_parent(notes, docs);
        index.set_parent(report, docs);
        index.set_parent(log, 0);
        index
    }

    fn run(query: &str, sort: &str) -> Vec<String> {
        let index = sample();
        let response = search(
            &index,
            &SearchRequest {
                query: query.to_string(),
                sort: sort.to_string(),
                ..Default::default()
            },
        );
        response.items.into_iter().map(|item| item.name).collect()
    }

    #[test]
    fn and_or_not_and_wildcards_work() {
        assert_eq!(run("notes", "name"), vec!["notes.txt"]);
        assert_eq!(run("NOTES", "name"), vec!["notes.txt"]);
        assert_eq!(run("notes | setup", "name"), vec!["setup.LOG", "notes.txt"]);
        assert_eq!(run("ext:pdf", "name"), vec!["report-final.pdf"]);
        assert_eq!(run("report !pdf", "name"), Vec::<String>::new());
        assert_eq!(run("report", "name"), vec!["report-final.pdf"]);
        assert_eq!(run("rep*", "name"), vec!["report-final.pdf"]);
        assert_eq!(run("path:docs", "name"), vec!["report-final.pdf", "notes.txt", "docs"]);
    }

    #[test]
    fn size_and_date_filters_work() {
        assert_eq!(run("size:>500kb", "name"), vec!["setup.LOG"]);
        assert_eq!(run("size:<2kb", "name"), vec!["notes.txt", "docs"]);
        let mut index = sample();
        index.version = 3;
        let response = search(
            &index,
            &SearchRequest {
                query: "dm:3650d".to_string(),
                sort: "size".to_string(),
                asc: true,
                ..Default::default()
            },
        );
        assert_eq!(response.total, 3);
        assert_eq!(response.version, 3);
    }

    #[test]
    fn pagination_reports_total_and_has_more() {
        let index = sample();
        let response = search(
            &index,
            &SearchRequest {
                query: "a".to_string(),
                limit: 1,
                sort: "name".to_string(),
                asc: true,
                ..Default::default()
            },
        );
        assert_eq!(response.items.len(), 1);
        assert!(response.total >= 1);
    }

    #[test]
    fn empty_query_without_scope_returns_nothing() {
        let index = sample();
        let response = search(&index, &SearchRequest::default());
        assert_eq!(response.total, 0);
    }

    #[test]
    fn invalid_field_values_are_reported() {
        let index = sample();
        let response = search(
            &index,
            &SearchRequest {
                query: "size:>huge".to_string(),
                ..Default::default()
            },
        );
        assert!(response.error.is_some());
        assert_eq!(response.total, 0);
    }

    fn matrix() -> Index {
        let mut index = Index::new("C");
        let docs = index.push_entry(10, 0, "docs", 0x10, 0, 0);
        index.push_entry(11, docs, "notes.txt", 0, 100, 1_700_000_000);
        index.push_entry(12, docs, "report-final.pdf", 0, 5_000, 1_700_000_100);
        index.push_entry(13, 0, "setup.LOG", 0, 1_000_000, 1_690_000_000);
        index.push_entry(14, 0, "fresh.tmp", 0, 2_000, now_secs() - 3_600);
        index.set_parent(docs, 0);
        index
    }

    fn matrix_run(index: &Index, query: &str) -> Vec<String> {
        let response = search(
            index,
            &SearchRequest {
                query: query.to_string(),
                sort: "name".to_string(),
                asc: true,
                limit: 100,
                ..Default::default()
            },
        );
        assert!(response.error.is_none(), "query {query} errored: {:?}", response.error);
        response.items.into_iter().map(|item| item.name).collect()
    }

    /// Prints and checks one documented example per supported syntax feature.
    #[test]
    fn search_syntax_matrix() {
        let index = matrix();
        let cases: &[(&str, &[&str])] = &[
            ("notes", &["notes.txt"]),
            ("NOTES", &["notes.txt"]),
            ("*.txt", &["notes.txt"]),
            ("report-?inal.pdf", &["report-final.pdf"]),
            ("ext:pdf", &["report-final.pdf"]),
            ("ext:pdf | ext:log", &["report-final.pdf", "setup.LOG"]),
            ("pdf | log", &["report-final.pdf", "setup.LOG"]),
            (
                "!ext:pdf",
                &["docs", "fresh.tmp", "notes.txt", "setup.LOG"],
            ),
            ("report !pdf", &[]),
            ("!notes", &["docs", "fresh.tmp", "report-final.pdf", "setup.LOG"]),
            ("size:>500kb", &["setup.LOG"]),
            ("size:<2kb", &["docs", "fresh.tmp", "notes.txt"]),
            ("size:>=900kb", &["setup.LOG"]),
            ("dm:1d", &["fresh.tmp"]),
            (
                "path:docs",
                &["docs", "notes.txt", "report-final.pdf"],
            ),
            ("path:docs size:>1kb", &["report-final.pdf"]),
        ];
        for (query, expected) in cases {
            let names = matrix_run(&index, query);
            println!("{query:<22} => {names:?}");
            let expected: Vec<String> = expected.iter().map(|name| name.to_string()).collect();
            assert_eq!(names, expected, "query: {query}");
        }
    }

    #[test]
    fn pasted_full_path_matches_by_path_semantics() {
        let index = matrix();
        let names = matrix_run(&index, r"C:\docs\notes.txt");
        assert_eq!(names, vec!["notes.txt"]);
    }

    #[test]
    fn top_k_page_matches_a_full_sort() {
        let mut index = Index::new("C");
        for position in 0..20_000u64 {
            let dir = index.push_entry(1000 + position / 100, 0, &format!("d{}", position / 100), 0x10, 0, 0);
            index.set_parent(dir, 0);
            index.push_entry(
                100_000 + position,
                dir,
                &format!("file{}x.bin", position),
                0,
                0x10_0000 + (position * 7919) % 50_000,
                (position * 31) as i64,
            );
        }
        let request = |sort: &str, offset: usize| SearchRequest {
            query: "x".to_string(),
            sort: sort.to_string(),
            offset,
            limit: 50,
            ..Default::default()
        };
        for sort in ["size", "mtime", "name", "path"] {
            for offset in [0usize, 499] {
                let response = search(&index, &request(sort, offset));
                let mut all: Vec<u32> = (1..index.len() as u32)
                    .filter(|idx| index.name(*idx).contains('x'))
                    .collect();
                let asc = false;
                all.sort_by(|a, b| compare_slot(&index, *a, *b, sort, asc));
                let expected: Vec<String> = all[offset..offset + 50]
                    .iter()
                    .map(|idx| index.name(*idx).to_string())
                    .collect();
                let actual: Vec<String> = response.items.iter().map(|item| item.name.clone()).collect();
                assert_eq!(actual, expected, "sort={sort} offset={offset}");
                assert_eq!(response.total, all.len());
            }
        }
    }

    #[test]
    fn cancelled_search_returns_quickly() {
        let index = matrix();
        let cancel = AtomicBool::new(true);
        let started = std::time::Instant::now();
        let response = search_with_cancel(
            &index,
            &SearchRequest {
                query: "e".to_string(),
                ..Default::default()
            },
            Some(&cancel),
        );
        assert_eq!(response.total, 0);
        assert!(started.elapsed().as_millis() < 50);
    }

    #[test]
    fn path_comparator_matches_path_of() {
        let index = matrix();
        let ids: Vec<u32> = (0..index.len() as u32).collect();
        for &left in &ids {
            for &right in &ids {
                let expected = compare_text(&index.path_of(left), &index.path_of(right), true);
                assert_eq!(
                    compare_paths(&index, left, right),
                    expected,
                    "{} vs {}",
                    index.path_of(left),
                    index.path_of(right)
                );
            }
        }
    }

    #[test]
    fn wildcard_ascii_matches_legacy_chars() {
        assert!(wildcard_bytes(b"report-final.pdf", b"report-?inal.pdf", false));
        assert!(wildcard_bytes(b"Report-Final.PDF", b"report-*", false));
        assert!(!wildcard_bytes(b"notes.txt", b"*.pdf", false));
    }
}
