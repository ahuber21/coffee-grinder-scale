import { useEffect, useState } from "react";
import TareCalibrationPage from "./advanced/TareCalibration";
import CalibrationPage from "./advanced/Calibration";
import ModelPage from "./advanced/Model";

type SubTab = "tare" | "calibration" | "model";

function subTabFromHash(hash: string): SubTab {
  const clean = hash.replace(/^#\/?/, "");
  const parts = clean.split("/");
  if (parts[1] === "calibration" || parts[1] === "model") {
    return parts[1];
  }
  return "tare";
}

/** Advanced tab shell: owns the #/advanced/<sub> sub-route, not App's concern. */
export default function AdvancedPage() {
  const [sub, setSub] = useState<SubTab>(() => subTabFromHash(window.location.hash));

  useEffect(() => {
    const onHashChange = () => setSub(subTabFromHash(window.location.hash));
    window.addEventListener("hashchange", onHashChange);
    return () => window.removeEventListener("hashchange", onHashChange);
  }, []);

  return (
    <>
      <nav className="subtabs">
        <a href="#/advanced/tare" className={sub === "tare" ? "active" : ""}>
          Tare calibration
        </a>
        <a href="#/advanced/calibration" className={sub === "calibration" ? "active" : ""}>
          Calibration
        </a>
        <a href="#/advanced/model" className={sub === "model" ? "active" : ""}>
          Model
        </a>
      </nav>
      {sub === "tare" && <TareCalibrationPage />}
      {sub === "calibration" && <CalibrationPage />}
      {sub === "model" && <ModelPage />}
    </>
  );
}
