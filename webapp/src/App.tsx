import { useEffect, useState } from "react";
import { useDeviceSocket } from "./lib/DeviceSocketContext";
import LivePage from "./pages/Live";
import SettingsPage from "./pages/Settings";
import HistoryPage from "./pages/History";
import AdvancedPage from "./pages/Advanced";

// Four top-level tabs on a flat hash router. The Advanced tab's own sub-route
// (#/advanced/tare, ...) is parsed by AdvancedPage.
type Tab = "live" | "settings" | "history" | "advanced";

function tabFromHash(hash: string): Tab {
  const clean = hash.replace(/^#\/?/, "");
  const top = clean.split("/")[0];
  if (top === "settings" || top === "history" || top === "advanced") {
    return top;
  }
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
        <a href="#/advanced" className={tab === "advanced" ? "active" : ""}>
          Advanced
        </a>
      </nav>
      <div className="page-fade" key={tab}>
        {tab === "live" && <LivePage />}
        {tab === "settings" && <SettingsPage />}
        {tab === "history" && <HistoryPage />}
        {tab === "advanced" && <AdvancedPage />}
      </div>
    </div>
  );
}
