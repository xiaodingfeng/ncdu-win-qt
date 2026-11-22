import React from "react";
import ReactDOM from "react-dom/client";
import App from "./App";
import ErrorBoundary from "./components/ErrorBoundary";
import { installClientErrorLogging } from "./lib/log";
import { SettingsProvider } from "./context/SettingsContext";
import { I18nProvider } from "./context/I18nContext";
import { AppConfigProvider } from "./context/AppConfigContext";
import "./styles.css";

installClientErrorLogging();

ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
  <React.StrictMode>
    <ErrorBoundary>
      <AppConfigProvider>
        <SettingsProvider>
          <I18nProvider>
            <App />
          </I18nProvider>
        </SettingsProvider>
      </AppConfigProvider>
    </ErrorBoundary>
  </React.StrictMode>,
);
