import { useEffect, useState } from "react";
import { useDeviceSocket } from "./lib/DeviceSocketContext";
import LivePage from "./pages/Live";
import SettingsPage from "./pages/Settings";
import HistoryPage from "./pages/History";

// Three tabs, one flat hash router -- no react-router dependency needed
// for something this small (D4's ambition is the app itself, not the
// choice of routing library).
type Tab = "live" | "settings" | "history";

function tabFromHash(hash: string): Tab {
  const clean = hash.replace(/^#\/?/, "");
  if (clean === "settings" || clean === "history") return clean;
  return "live";
}

export default function App() {
  const [tab, setTab] = useState<Tab>(() => tabFromHash(window.location.hash));
  const { status } = useDeviceSocket();

  useEffect(() => {
    const onHashChange = () => setTab(tabFromHash(window.location.hash));
    window.addEventListener("hashchange", onHashChange);
    return () => window.removeEventListener("hashchange", onHashChange);
  }, []);

  return (
    <div className="app-shell">
      <h1>
        <span className={`status-dot ${status}`} title={status} />
        Eureka
      </h1>
      <nav className="tabs">
        <a href="#/live" className={tab === "live" ? "active" : ""}>
          Live
        </a>
        <a href="#/settings" className={tab === "settings" ? "active" : ""}>
          Settings
        </a>
        <a href="#/history" className={tab === "history" ? "active" : ""}>
          History
        </a>
      </nav>
      {tab === "live" && <LivePage />}
      {tab === "settings" && <SettingsPage />}
      {tab === "history" && <HistoryPage />}
    </div>
  );
}
