//! Query kernel: parses the Everything-style subset (AND / OR / NOT, wildcards,
//! `ext:` / `path:` / `size:` / `dm:`) and scans the compact index in parallel.

use std::cmp::Ordering;

use rayon::prelude::*;
use serde::Serialize;

use super::builder::now_secs;
use super::model::{fold_char, fold_contains, Index};

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

#[derive(Debug, Clone)]
enum Field {
    Name,
    Ext,
    Path,
    SizeAtLeast(u64),
    SizeAtMost(u64),
    MtimeAfter(i64),
}

#[derive(Debug, Clone)]
struct Term {
    negated: bool,
    field: Field,
    needle: String,
}

pub(crate) fn search(index: &Index, request: &SearchRequest) -> SearchResponse {
    let started = std::time::Instant::now();
    let (groups, parse_error) = parse_query(&request.query, request.case_sensitive);
    let scope = request
        .scope
        .as_ref()
        .map(|value| normalize_scope(value))
        .filter(|value| !value.is_empty());

    let empty = || SearchResponse {
        items: Vec::new(),
        total: 0,
        took_ms: started.elapsed().as_millis() as u64,
        version: index.version,
        has_more: false,
        error: parse_error.clone(),
    };
    if groups.is_empty() && scope.is_none() {
        return empty();
    }

    let keep = |idx: &u32| -> bool {
        if *idx == 0 || index.name(*idx).is_empty() || !index.is_live(*idx) {
            return false;
        }
        if let Some(scope) = &scope {
            if !fold_contains(&index.path_of(*idx), scope) {
                return false;
            }
        }
        matches_entry(index, *idx, &groups, request.case_sensitive)
    };
    // `ext:` lets us scan only the matching extension bucket instead of every
    // entry in the volume.
    let mut matched: Vec<u32> = match candidate_slice(index, &groups) {
        Some(candidates) => candidates
            .par_iter()
            .filter(|idx| keep(idx))
            .map(|idx| *idx)
            .collect(),
        None => (0..index.len() as u32)
            .into_par_iter()
            .filter(|idx| keep(idx))
            .collect(),
    };

    let total = matched.len();
    let asc = request.asc;
    match request.sort.as_str() {
        "name" => matched.sort_by(|left, right| {
            compare_text(index.name(*left), index.name(*right), asc)
        }),
        "path" => {
            let mut rows: Vec<(u32, String)> = matched
                .into_iter()
                .map(|idx| (idx, index.path_of(idx)))
                .collect();
            rows.sort_by(|left, right| compare_text(&left.1, &right.1, asc));
            matched = rows.into_iter().map(|(idx, _)| idx).collect();
        }
        "mtime" => matched.sort_by(|left, right| {
            order(
                index.mtimes[*left as usize].cmp(&index.mtimes[*right as usize]),
                asc,
            )
        }),
        _ => matched.sort_by(|left, right| {
            order(
                index.sizes[*left as usize].cmp(&index.sizes[*right as usize]),
                asc,
            )
        }),
    }

    let offset = request.offset.min(matched.len());
    let limit = request.limit.clamp(1, 1000);
    let end = (offset + limit).min(matched.len());
    let items: Vec<SearchItem> = matched[offset..end]
        .iter()
        .map(|idx| SearchItem {
            path: index.path_of(*idx),
            name: index.name(*idx).to_string(),
            size: index.sizes[*idx as usize],
            mtime: index.mtimes[*idx as usize],
            is_dir: index.is_dir(*idx),
            hidden: index.is_hidden(*idx),
        })
        .collect();

    SearchResponse {
        has_more: end < total,
        items,
        total,
        took_ms: started.elapsed().as_millis() as u64,
        version: index.version,
        error: parse_error,
    }
}

/// When a single AND group contains an `ext:` term, restrict the scan to that
/// extension bucket. Negated terms already scan everything, so they opt out.
fn candidate_slice<'a>(index: &'a Index, groups: &[Vec<Term>]) -> Option<&'a [u32]> {
    if groups.len() != 1 {
        return None;
    }
    let group = groups.first()?;
    if group.iter().any(|term| term.negated) {
        return None;
    }
    let extension = group.iter().find_map(|term| match &term.field {
        Field::Ext => Some(term.needle.as_str()),
        _ => None,
    })?;
    index.ext_bucket(extension)
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

fn compare_text(left: &str, right: &str, asc: bool) -> Ordering {
    let mut left_chars = left.chars();
    let mut right_chars = right.chars();
    loop {
        match (left_chars.next(), right_chars.next()) {
            (None, None) => return Ordering::Equal,
            (None, Some(_)) => return order(Ordering::Less, asc),
            (Some(_), None) => return order(Ordering::Greater, asc),
            (Some(left_char), Some(right_char)) => {
                let comparison = fold_char(left_char).cmp(&fold_char(right_char));
                if comparison != Ordering::Equal {
                    return order(comparison, asc);
                }
            }
        }
    }
}

fn matches_entry(index: &Index, idx: u32, groups: &[Vec<Term>], case: bool) -> bool {
    if groups.is_empty() {
        return true;
    }
    groups.iter().any(|terms| {
        terms
            .iter()
            .all(|term| term.negated != term_hits(index, idx, term, case))
    })
}

fn term_hits(index: &Index, idx: u32, term: &Term, case: bool) -> bool {
    match &term.field {
        Field::Name => name_hits(index.name(idx), &term.needle, case),
        Field::Ext => index.ext(idx) == term.needle,
        Field::Path => fold_contains(&index.path_of(idx), &term.needle),
        Field::SizeAtLeast(min) => index.sizes[idx as usize] >= *min,
        Field::SizeAtMost(max) => index.sizes[idx as usize] <= *max,
        Field::MtimeAfter(after) => index.mtimes[idx as usize] >= *after,
    }
}

fn name_hits(name: &str, needle: &str, case: bool) -> bool {
    if needle.contains('*') || needle.contains('?') {
        wildcard_match(name, needle, case)
    } else if case {
        name.contains(needle)
    } else {
        fold_contains(name, needle)
    }
}

fn wildcard_match(text: &str, pattern: &str, case: bool) -> bool {
    let text: Vec<char> = text.chars().collect();
    let pattern: Vec<char> = pattern.chars().collect();
    let (mut text_pos, mut pattern_pos) = (0usize, 0usize);
    let (mut star, mut resume) = (None, 0usize);
    let equals = |left: char, right: char| {
        if case {
            left == right
        } else {
            fold_char(left) == fold_char(right)
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

fn parse_query(query: &str, case: bool) -> (Vec<Vec<Term>>, Option<String>) {
    let mut groups: Vec<Vec<Term>> = vec![Vec::new()];
    let mut error: Option<String> = None;
    for token in tokenize(query) {
        if token == "|" {
            if groups.last().map(|group| !group.is_empty()).unwrap_or(false) {
                groups.push(Vec::new());
            }
            continue;
        }
        match parse_term(&token, case) {
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

fn parse_term(token: &str, case: bool) -> Result<Option<Term>, String> {
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
            field: Field::Ext,
            needle: extension.to_string(),
        }));
    }
    if let Some(value) = lower.strip_prefix("path:") {
        let value = value.trim();
        if value.is_empty() {
            return Err("path: needs a fragment".to_string());
        }
        return Ok(Some(Term {
            negated,
            field: Field::Path,
            needle: value.to_string(),
        }));
    }
    if let Some(value) = lower.strip_prefix("size:") {
        let value = value.trim();
        let field = parse_size(value)
            .ok_or_else(|| format!("invalid size filter: size:{value}"))?;
        return Ok(Some(Term {
            negated,
            field,
            needle: String::new(),
        }));
    }
    if let Some(value) = lower.strip_prefix("dm:") {
        let value = value.trim();
        let after = parse_age(value)
            .ok_or_else(|| format!("invalid date filter: dm:{value}"))?;
        return Ok(Some(Term {
            negated,
            field: Field::MtimeAfter(after),
            needle: String::new(),
        }));
    }
    Ok(Some(Term {
        negated,
        field: Field::Name,
        needle: if case { text.to_string() } else { lower },
    }))
}

fn parse_size(value: &str) -> Option<Field> {
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
        ">" => Field::SizeAtLeast(bytes.saturating_add(1)),
        "<" => Field::SizeAtMost(bytes.saturating_sub(1)),
        _ => Field::SizeAtLeast(bytes),
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
}
