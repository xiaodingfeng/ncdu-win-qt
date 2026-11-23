import { useRef, useState } from "react";
import AnchoredPopup from "./AnchoredPopup";
import type { SelectOption } from "./Select";

interface MultiSelectProps {
  values: string[];
  options: SelectOption[];
  allLabel: string;
  onChange: (values: string[]) => void;
  className?: string;
  title?: string;
  ariaLabel?: string;
}

/**
 * 多选下拉：`values` 为空表示「全部」，点击选项可连续勾选，浮层不会自动收起。
 */
export default function MultiSelect({
  values,
  options,
  allLabel,
  onChange,
  className = "",
  title,
  ariaLabel,
}: MultiSelectProps) {
  const [open, setOpen] = useState(false);
  const anchorRef = useRef<HTMLButtonElement | null>(null);
  const selected = options.filter((option) => values.includes(option.value));
  const label =
    selected.length === 0
      ? allLabel
      : selected.length === 1
        ? selected[0].label
        : `${selected[0].label} +${selected.length - 1}`;

  const toggle = (value: string) => {
    onChange(values.includes(value) ? values.filter((item) => item !== value) : [...values, value]);
  };

  return (
    <>
      <button
        type="button"
        ref={anchorRef}
        className={`select ${open ? "open" : ""} ${className}`}
        title={title}
        aria-label={ariaLabel}
        aria-haspopup="listbox"
        aria-expanded={open}
        onClick={() => setOpen((prev) => !prev)}
      >
        <span className="select-label">{label}</span>
        <svg
          className="select-chevron"
          viewBox="0 0 24 24"
          fill="none"
          stroke="currentColor"
          strokeWidth="2"
          strokeLinecap="round"
          strokeLinejoin="round"
          aria-hidden
        >
          <path d="M6 9l6 6 6-6" />
        </svg>
      </button>
      <AnchoredPopup open={open} onClose={() => setOpen(false)} anchorRef={anchorRef}>
        <div className="select-list" role="listbox" aria-multiselectable>
          <button
            type="button"
            className={`select-option ${values.length === 0 ? "active" : ""}`}
            onClick={() => onChange([])}
          >
            {allLabel}
          </button>
          {options.map((option) => {
            const active = values.includes(option.value);
            return (
              <button
                key={option.value}
                type="button"
                role="option"
                aria-selected={active}
                className={`select-option multi ${active ? "active" : ""}`}
                onClick={() => toggle(option.value)}
              >
                <span className="select-check" aria-hidden>
                  {active ? "✓" : ""}
                </span>
                {option.label}
              </button>
            );
          })}
        </div>
      </AnchoredPopup>
    </>
  );
}
