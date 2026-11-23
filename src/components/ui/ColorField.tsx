import { useEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from "react";
import AnchoredPopup from "./AnchoredPopup";

const PRESETS = ["#0f172a", "#475569", "#94a3b8", "#ffffff", "#14b8a6", "#2563eb", "#f59e0b", "#ef4444"];

const clamp01 = (value: number) => Math.min(1, Math.max(0, value));

function hexToRgb(hex: string): [number, number, number] {
  const text = hex.replace("#", "");
  const full = text.length === 3 ? text.split("").map((char) => char + char).join("") : text;
  const value = Number.parseInt(full, 16);
  if (full.length !== 6 || !Number.isFinite(value)) return [0, 0, 0];
  return [(value >> 16) & 255, (value >> 8) & 255, value & 255];
}

function rgbToHex(r: number, g: number, b: number) {
  return `#${[r, g, b].map((value) => Math.round(value).toString(16).padStart(2, "0")).join("")}`;
}

function rgbToHsv(r: number, g: number, b: number): [number, number, number] {
  const rn = r / 255;
  const gn = g / 255;
  const bn = b / 255;
  const max = Math.max(rn, gn, bn);
  const delta = max - Math.min(rn, gn, bn);
  let h = 0;
  if (delta !== 0) {
    if (max === rn) h = ((gn - bn) / delta) % 6;
    else if (max === gn) h = (bn - rn) / delta + 2;
    else h = (rn - gn) / delta + 4;
    h *= 60;
    if (h < 0) h += 360;
  }
  return [h, max === 0 ? 0 : delta / max, max];
}

function hsvToRgb(h: number, s: number, v: number): [number, number, number] {
  const c = v * s;
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
  const m = v - c;
  const table: [number, number, number][] = [
    [c, x, 0],
    [x, c, 0],
    [0, c, x],
    [0, x, c],
    [x, 0, c],
    [c, 0, x],
  ];
  const [r, g, b] = table[Math.floor(h / 60) % 6];
  return [(r + m) * 255, (g + m) * 255, (b + m) * 255];
}

interface ColorFieldProps {
  value: string;
  onChange: (value: string) => void;
  title?: string;
}

/**
 * 自定义取色控件，替代原生 `<input type="color">`：色相条 + 明度/饱和度面板
 * + 十六进制输入 + 常用色，浮层与下拉共用 `AnchoredPopup` 的淡入淡出。
 */
export default function ColorField({ value, onChange, title }: ColorFieldProps) {
  const [open, setOpen] = useState(false);
  const anchorRef = useRef<HTMLButtonElement | null>(null);
  const areaRef = useRef<HTMLDivElement | null>(null);
  const hueRef = useRef<HTMLDivElement | null>(null);
  const [hsv, setHsv] = useState<[number, number, number]>(() => rgbToHsv(...hexToRgb(value)));
  const [hexText, setHexText] = useState(value);
  const lastCommitted = useRef(value);

  useEffect(() => {
    // 自己提交的颜色不必回灌，避免拖动时来回取整导致抖动。
    if (value === lastCommitted.current) return;
    lastCommitted.current = value;
    setHsv(rgbToHsv(...hexToRgb(value)));
    setHexText(value);
  }, [value]);

  const commit = (next: [number, number, number]) => {
    setHsv(next);
    const hex = rgbToHex(...hsvToRgb(...next));
    lastCommitted.current = hex;
    setHexText(hex);
    onChange(hex);
  };

  const pickArea = (event: ReactPointerEvent<HTMLDivElement>) => {
    const area = areaRef.current;
    if (!area) return;
    const rect = area.getBoundingClientRect();
    commit([
      hsv[0],
      clamp01((event.clientX - rect.left) / rect.width),
      1 - clamp01((event.clientY - rect.top) / rect.height),
    ]);
  };

  const pickHue = (event: ReactPointerEvent<HTMLDivElement>) => {
    const hue = hueRef.current;
    if (!hue) return;
    const rect = hue.getBoundingClientRect();
    commit([clamp01((event.clientX - rect.left) / rect.width) * 359.9, hsv[1], hsv[2]]);
  };

  const drag = (handler: (event: ReactPointerEvent<HTMLDivElement>) => void) => ({
    onPointerDown: (event: ReactPointerEvent<HTMLDivElement>) => {
      event.currentTarget.setPointerCapture(event.pointerId);
      handler(event);
    },
    onPointerMove: (event: ReactPointerEvent<HTMLDivElement>) => {
      if (event.currentTarget.hasPointerCapture(event.pointerId)) handler(event);
    },
  });

  const [h, s, v] = hsv;

  return (
    <>
      <button
        type="button"
        ref={anchorRef}
        className="color-swatch"
        style={{ background: rgbToHex(...hsvToRgb(h, s, v)) }}
        title={title ?? value}
        aria-label={title ?? value}
        aria-haspopup="dialog"
        aria-expanded={open}
        onClick={() => setOpen((prev) => !prev)}
      />
      <AnchoredPopup
        className="color-popup"
        open={open}
        onClose={() => setOpen(false)}
        anchorRef={anchorRef}
      >
        <div className="color-panel">
          <div
            ref={areaRef}
            className="color-area"
            style={{
              background: `linear-gradient(to top, #000, rgba(0, 0, 0, 0)), linear-gradient(to right, #fff, hsl(${Math.round(h)}, 100%, 50%))`,
            }}
            {...drag(pickArea)}
          >
            <span
              className="color-area-knob"
              style={{ left: `${s * 100}%`, top: `${(1 - v) * 100}%` }}
            />
          </div>
          <div ref={hueRef} className="color-hue" {...drag(pickHue)}>
            <span className="color-hue-knob" style={{ left: `${(h / 360) * 100}%` }} />
          </div>
          <input
            className="input color-hex"
            value={hexText}
            spellCheck={false}
            onChange={(event) => {
              const text = event.target.value;
              setHexText(text);
              if (!/^#[0-9a-f]{6}$/i.test(text)) return;
              const [r, g, b] = hexToRgb(text);
              lastCommitted.current = text.toLowerCase();
              setHsv(rgbToHsv(r, g, b));
              onChange(text.toLowerCase());
            }}
          />
          <div className="color-presets">
            {PRESETS.map((preset) => (
              <button
                key={preset}
                type="button"
                className="color-preset"
                style={{ background: preset }}
                title={preset}
                onClick={() => commit(rgbToHsv(...hexToRgb(preset)))}
              />
            ))}
          </div>
        </div>
      </AnchoredPopup>
    </>
  );
}
