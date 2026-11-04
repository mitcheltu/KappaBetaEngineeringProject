import { useLayoutEffect, useRef, type ReactNode } from 'react';

type SheetFrameProps = {
  open: boolean;
  onClose: () => void;
  children: ReactNode;
};

/** Browser-native modal semantics provide focus containment and background inertness. */
export default function SheetFrame({ open, onClose, children }: SheetFrameProps) {
  const dialogRef = useRef<HTMLDialogElement>(null);
  const backdropPress = useRef(false);

  useLayoutEffect(() => {
    const dialog = dialogRef.current;
    if (!open || !dialog) return;

    const opener = document.activeElement instanceof HTMLElement
      ? document.activeElement
      : null;
    const scrollX = window.scrollX;
    const scrollY = window.scrollY;
    const body = document.body;
    const previous = {
      overflow: body.style.overflow,
      position: body.style.position,
      top: body.style.top,
      left: body.style.left,
      right: body.style.right,
      width: body.style.width,
    };

    // A fixed body also prevents background scrolling in mobile Safari.
    body.style.overflow = 'hidden';
    body.style.position = 'fixed';
    body.style.top = `${-scrollY}px`;
    body.style.left = `${-scrollX}px`;
    body.style.right = '0';
    body.style.width = '100%';
    if (!dialog.open) dialog.showModal();

    const initialFocus = dialog.querySelector<HTMLElement>(
      'button:not([disabled]), [role="button"]:not([aria-disabled="true"]), [tabindex]:not([tabindex="-1"])',
    );
    (initialFocus ?? dialog).focus({ preventScroll: true });

    return () => {
      if (dialog.open) dialog.close();
      Object.assign(body.style, previous);
      window.scrollTo({ left: scrollX, top: scrollY, behavior: 'instant' });
      // A successful connection can replace the original Connect control.
      const fallback = document.querySelector<HTMLElement>(
        '[data-testid="connection-action"], main button, main [role="button"], [role="main"] button, [role="main"] [role="button"]',
      );
      const restore = opener?.isConnected ? opener : fallback;
      restore?.focus({ preventScroll: true });
      backdropPress.current = false;
    };
  }, [open]);

  return (
    <dialog
      ref={dialogRef}
      className="device-dialog"
      aria-label="Connect demo device"
      aria-modal="true"
      tabIndex={-1}
      onKeyDown={(event) => {
        if (event.key !== 'Tab') return;
        const dialog = event.currentTarget;
        const controls = Array.from(dialog.querySelectorAll<HTMLElement>(
          'button, input, select, textarea, a[href], [tabindex]:not([tabindex="-1"])',
        )).filter((element) => !element.hasAttribute('disabled') && element.getAttribute('aria-disabled') !== 'true' && element.getClientRects().length > 0);
        const first = controls[0];
        const last = controls[controls.length - 1];
        if (!first) {
          event.preventDefault();
          dialog.focus();
        } else if (event.shiftKey && (document.activeElement === first || document.activeElement === dialog)) {
          event.preventDefault();
          last.focus();
        } else if (!event.shiftKey && (document.activeElement === last || document.activeElement === dialog)) {
          event.preventDefault();
          first.focus();
        }
      }}
      onCancel={(event) => {
        // React state owns dismissal; the native close event is not another
        // close request, so successful connections do not get canceled.
        event.preventDefault();
        onClose();
      }}
      onPointerDown={(event) => {
        const rect = event.currentTarget.getBoundingClientRect();
        backdropPress.current = event.target === event.currentTarget && (
          event.clientX < rect.left || event.clientX > rect.right ||
          event.clientY < rect.top || event.clientY > rect.bottom
        );
      }}
      onClick={(event) => {
        const rect = event.currentTarget.getBoundingClientRect();
        const outside = event.clientX < rect.left || event.clientX > rect.right ||
          event.clientY < rect.top || event.clientY > rect.bottom;
        if (backdropPress.current && event.target === event.currentTarget && outside) onClose();
        backdropPress.current = false;
      }}
    >
      {children}
    </dialog>
  );
}
