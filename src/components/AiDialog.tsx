import { useCallback, useEffect, useRef, useState } from "react";
import { useI18n } from "../context/I18nContext";
import Modal from "./Modal";
import Fade from "./Fade";
import { renderMarkdown } from "../lib/markdown";
import { aiCancel, aiChat, aiGetConfig, subscribeAi } from "../lib/ai";
import type { ChatMessage } from "../types/ai";

interface Props {
  open: boolean;
  subject?: string;
  systemPrompt?: string;
  initialPrompt?: string;
  onClose: () => void;
  onConfigure: () => void;
}

interface Turn {
  role: "user" | "assistant";
  content: string;
}

export default function AiDialog({
  open,
  subject,
  systemPrompt,
  initialPrompt,
  onClose,
  onConfigure,
}: Props) {
  const { t } = useI18n();
  const [turns, setTurns] = useState<Turn[]>([]);
  const [buffer, setBuffer] = useState("");
  const [streaming, setStreaming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [configured, setConfigured] = useState<boolean | null>(null);
  const [input, setInput] = useState("");
  const bufferRef = useRef("");
  const wasOpen = useRef(false);
  const viewRef = useRef<HTMLDivElement>(null);

  const begin = useCallback(
    async (history: Turn[]) => {
      setError(null);
      setBuffer("");
      bufferRef.current = "";
      setStreaming(true);
      const messages: ChatMessage[] = [];
      if (systemPrompt) messages.push({ role: "system", content: systemPrompt });
      for (const turn of history) messages.push({ role: turn.role, content: turn.content });
      try {
        await aiChat(messages);
      } catch (err) {
        setStreaming(false);
        setError(String(err));
      }
    },
    [systemPrompt],
  );

  useEffect(() => {
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeAi({
      onChunk: (payload) => {
        if (disposed) return;
        bufferRef.current += payload.delta;
        setBuffer((previous) => previous + payload.delta);
      },
      onDone: () => {
        if (disposed) return;
        const text = bufferRef.current;
        bufferRef.current = "";
        setBuffer("");
        setStreaming(false);
        if (text) setTurns((previous) => [...previous, { role: "assistant", content: text }]);
      },
      onError: (payload) => {
        if (disposed) return;
        bufferRef.current = "";
        setBuffer("");
        setStreaming(false);
        setError(payload.message);
      },
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, []);

  useEffect(() => {
    if (!open) {
      wasOpen.current = false;
      return;
    }
    if (wasOpen.current) return;
    wasOpen.current = true;
    setTurns([]);
    setBuffer("");
    setInput("");
    setError(null);
    setStreaming(false);
    void (async () => {
      try {
        const config = await aiGetConfig();
        const ready = !!config.baseUrl && config.hasApiKey;
        setConfigured(ready);
        if (!ready) return;
        if (initialPrompt) {
          const history: Turn[] = [{ role: "user", content: initialPrompt }];
          setTurns(history);
          await begin(history);
        }
      } catch (err) {
        setConfigured(false);
        setError(String(err));
      }
    })();
  }, [open, initialPrompt, begin]);

  useEffect(() => {
    const view = viewRef.current;
    if (view) view.scrollTop = view.scrollHeight;
  }, [turns, buffer, streaming]);

  const send = () => {
    const text = input.trim();
    if (!text || streaming) return;
    setInput("");
    const history: Turn[] = [...turns, { role: "user", content: text }];
    setTurns(history);
    void begin(history);
  };

  const copy = () => {
    const text = turns.map((turn) => turn.content).join("\n\n") + (buffer ? `\n\n${buffer}` : "");
    void navigator.clipboard.writeText(text);
  };

  return (
    <Fade show={open}>
      <Modal
        title={subject ? `${t("ai.analysis.title")} — ${subject}` : t("ai.analysis.title")}
        width={860}
        onClose={() => {
          if (streaming) void aiCancel();
          onClose();
        }}
        footer={
          <>
            <button className="btn" onClick={copy} disabled={turns.length === 0 && !buffer}>
              {t("ai.analysis.copy")}
            </button>
            <div className="toolbar-spacer" />
            {streaming && (
              <button className="btn" onClick={() => void aiCancel()}>
                {t("button.cancel")}
              </button>
            )}
            <button
              className="btn primary"
              onClick={() => {
                if (streaming) void aiCancel();
                onClose();
              }}
            >
              {t("button.close")}
            </button>
          </>
        }
      >
        <div className="ai-dialog">
          {configured === false ? (
            <div className="ai-empty">
              <p className="dialog-text">{t("ai.analysis.not_configured")}</p>
              <button className="btn primary" onClick={onConfigure}>
                {t("ai.analysis.configure")}
              </button>
            </div>
          ) : (
            <>
              <div className="ai-chat" ref={viewRef}>
                {turns.length === 0 && !streaming && !buffer && (
                  <div className="ai-placeholder">{t("ai.analysis.input_placeholder")}</div>
                )}
                {turns.map((turn, index) => (
                  <div className={`ai-turn ${turn.role}`} key={`${turn.role}-${index}`}>
                    <div className="ai-role">
                      {turn.role === "user" ? t("ai.analysis.you") : t("ai.analysis.assistant")}
                    </div>
                    {turn.role === "assistant" ? (
                      <div
                        className="ai-content ai-markdown"
                        dangerouslySetInnerHTML={{ __html: renderMarkdown(turn.content) }}
                      />
                    ) : (
                      <div className="ai-content">{turn.content}</div>
                    )}
                  </div>
                ))}
                {streaming && (
                  <div className="ai-turn assistant">
                    <div className="ai-role">{t("ai.analysis.assistant")}</div>
                    {buffer ? (
                      <div
                        className="ai-content ai-markdown"
                        dangerouslySetInnerHTML={{ __html: renderMarkdown(buffer) }}
                      />
                    ) : (
                      <div className="ai-content muted">{t("ai.analysis.analyzing")}</div>
                    )}
                  </div>
                )}
                {error && <div className="ai-error">{t("ai.analysis.failed", { error })}</div>}
              </div>
              <div className="ai-input-row">
                <input
                  className="input"
                  value={input}
                  placeholder={t("ai.analysis.input_placeholder")}
                  onChange={(event) => setInput(event.target.value)}
                  onKeyDown={(event) => {
                    if (event.key === "Enter" && !event.shiftKey) {
                      event.preventDefault();
                      send();
                    }
                  }}
                />
                <button className="btn primary" disabled={streaming || !input.trim()} onClick={send}>
                  {t("ai.analysis.send")}
                </button>
              </div>
            </>
          )}
        </div>
      </Modal>
    </Fade>
  );
}
