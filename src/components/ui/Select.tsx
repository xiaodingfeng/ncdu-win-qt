import { useRef, useState } from "react";
import AnchoredPopup from "./AnchoredPopup";

export interface SelectOption {
  value: string;
  label: string;
}

interface SelectProps {
  value: string;
  options: SelectOption[];
  onChange: (value: string) => void;
  className?: string;
  placeholder?: string;
  disabled?: boolean;
  title?: string;
  ariaLabel?: string;
}

/**
 * 统一的下拉选择控件，替代原生 `<select>`：外观沿用 `.select` 样式，
 * 展开列表走 `AnchoredPopup`，因此和弹框一样带淡入淡出。
 */
export default function Select({
  value,
  options,
  onChange,
  className = "",
  placeholder = "",
  disabled = false,
  title,
  ariaLabel,
}: SelectProps) {
  const [open, setOpen] = useState(false);
  const anchorRef = useRef<HTMLButtonElement | null>(null);
  const current = options.find((option) => option.value === value);

  return (
    <>
      <button
        type="button"
        ref={anchorRef}
        className={`select ${open ? "open" : ""} ${className}`}
        disabled={disabled}
        title={title}
        aria-label={ariaLabel}
        aria-haspopup="listbox"
        aria-expanded={open}
        onClick={() => setOpen((prev) => !prev)}
      >
        <span className="select-label">{current ? current.label : placeholder}</span>
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
        <div className="select-list" role="listbox">
          {options.map((option) => (
            <button
              key={option.value}
              type="button"
              role="option"
              aria-selected={option.value === value}
              className={`select-option ${option.value === value ? "active" : ""}`}
              onClick={() => {
                onChange(option.value);
                setOpen(false);
              }}
            >
              {option.label}
            </button>
          ))}
        </div>
      </AnchoredPopup>
    </>
  );
}
