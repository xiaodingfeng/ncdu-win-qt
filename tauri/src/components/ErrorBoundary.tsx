import { Component, type ErrorInfo, type ReactNode } from "react";
import { logClient } from "../lib/log";

interface Props {
  children: ReactNode;
}

interface State {
  error: Error | null;
}

/** Keeps a rendering crash from blanking the whole window. */
export default class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null };

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    logClient(
      "error",
      `react crash: ${error.message}\n${error.stack ?? ""}\n${info.componentStack ?? ""}`,
    );
  }

  render() {
    if (!this.state.error) return this.props.children;
    return (
      <div className="crash-screen">
        <div className="crash-card">
          <h2>应用遇到异常 / Something went wrong</h2>
          <p className="crash-message">{this.state.error.message}</p>
          <pre className="crash-detail mono">{this.state.error.stack}</pre>
          <p className="crash-hint">详细信息已写入应用日志文件。</p>
          <div className="crash-actions">
            <button className="btn" onClick={() => this.setState({ error: null })}>
              重试 / Retry
            </button>
            <button className="btn primary" onClick={() => window.location.reload()}>
              重新加载 / Reload
            </button>
          </div>
        </div>
      </div>
    );
  }
}
