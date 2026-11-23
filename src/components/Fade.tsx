import { useEffect, useRef, useState, type ReactNode } from "react";

interface FadeProps {
  show: boolean;
  children: ReactNode;
  /** Fade duration in ms; keep in sync with `.fade-layer` in styles.css. */
  duration?: number;
}

/**
 * Keeps its children mounted for one short fade-out after `show` turns false,
 * so dialogs, context menus and dropdowns animate both ways. Children handed in
 * as `null` (the usual `{state && <Dialog/>}` pattern) keep their last rendered
 * content while the exit plays. The wrapper is unstyled, so fixed-position
 * overlays and absolutely-positioned menus keep their layout.
 */
export default function Fade({ show, children, duration = 140 }: FadeProps) {
  const [mounted, setMounted] = useState(show);
  // Always start transparent: an element that mounts already visible (a
  // context menu, a toast) should fade in instead of popping.
  const [visible, setVisible] = useState(false);
  const lastChildren = useRef<ReactNode>(children);
  if (show && children) {
    lastChildren.current = children;
  }

  useEffect(() => {
    if (show) {
      setMounted(true);
      return;
    }
    setVisible(false);
    const timer = window.setTimeout(() => setMounted(false), duration);
    return () => window.clearTimeout(timer);
  }, [show, duration]);

  useEffect(() => {
    if (!mounted || !show) return;
    // Paint the closed state once so the transition has a starting point.
    const frame = window.requestAnimationFrame(() => setVisible(true));
    return () => window.cancelAnimationFrame(frame);
  }, [mounted, show]);

  if (!mounted) return null;
  return (
    <div className={`fade-layer ${visible ? "is-visible" : ""}`}>
      {show ? children : lastChildren.current}
    </div>
  );
}
