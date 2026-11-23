import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type CSSProperties,
  type ReactNode,
  type RefObject,
} from "react";
import { createPortal } from "react-dom";
import Fade from "../Fade";

interface AnchoredPopupProps {
  open: boolean;
  onClose: () => void;
  anchorRef: RefObject<HTMLElement | null>;
  align?: "start" | "end";
  minWidth?: number;
  className?: string;
  children: ReactNode;
}

/**
 * 自定义 UI 组件共用的下拉浮层：贴合触发元素下方、渲染到 body 避免被弹框
 * 的滚动/裁剪区切断，并与弹框共用 `Fade` 的淡入淡出。
 */
export default function AnchoredPopup({
  open,
  onClose,
  anchorRef,
  align = "start",
  minWidth,
  className = "",
  children,
}: AnchoredPopupProps) {
  const popupRef = useRef<HTMLDivElement | null>(null);
  const [style, setStyle] = useState<CSSProperties>({ visibility: "hidden" });

  const place = useCallback(() => {
    const anchor = anchorRef.current;
    if (!anchor) return;
    const rect = anchor.getBoundingClientRect();
    const width = Math.max(minWidth ?? rect.width, rect.width);
    const left = align === "end" ? rect.right - width : rect.left;
    setStyle({
      top: rect.bottom + 4,
      left: Math.max(8, Math.min(left, window.innerWidth - width - 8)),
      minWidth: width,
      visibility: "visible",
    });
  }, [align, anchorRef, minWidth]);

  useLayoutEffect(() => {
    if (open) place();
  }, [open, place]);

  useEffect(() => {
    if (!open) return;
    const onPointerDown = (event: MouseEvent) => {
      const target = event.target as Node;
      if (popupRef.current?.contains(target) || anchorRef.current?.contains(target)) return;
      onClose();
    };
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== "Escape") return;
      event.stopPropagation();
      onClose();
    };
    document.addEventListener("mousedown", onPointerDown, true);
    document.addEventListener("keydown", onKeyDown, true);
    window.addEventListener("resize", place);
    window.addEventListener("scroll", place, true);
    return () => {
      document.removeEventListener("mousedown", onPointerDown, true);
      document.removeEventListener("keydown", onKeyDown, true);
      window.removeEventListener("resize", place);
      window.removeEventListener("scroll", place, true);
    };
  }, [anchorRef, onClose, open, place]);

  return createPortal(
    <Fade show={open}>
      <div ref={popupRef} className={`ui-popup ${className}`} style={style}>
        {children}
      </div>
    </Fade>,
    document.body,
  );
}
