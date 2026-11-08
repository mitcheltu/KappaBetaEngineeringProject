import { useEffect, useState } from 'react';

type Theme = 'light' | 'dark';
const key = 'kappa-beta.theme.v1';
function storedTheme(): Theme | null {
  try {
    const value = localStorage.getItem(key);
    return value === 'light' || value === 'dark' ? value : null;
  } catch { return null; }
}
function systemTheme(): Theme {
  return typeof window !== 'undefined' && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
}

export function useTheme() {
  const [preference, setPreference] = useState<Theme | null>(storedTheme);
  const [system, setSystem] = useState<Theme>(systemTheme);
  const theme = preference ?? system;
  useEffect(() => {
    const query = window.matchMedia('(prefers-color-scheme: dark)');
    const update = () => setSystem(query.matches ? 'dark' : 'light');
    const sync = (event: StorageEvent) => { if (event.key === key || event.key === null) setPreference(storedTheme()); };
    update();
    query.addEventListener('change', update);
    window.addEventListener('storage', sync);
    return () => { query.removeEventListener('change', update); window.removeEventListener('storage', sync); };
  }, []);
  useEffect(() => { document.documentElement.dataset.theme = theme; }, [theme]);
  function toggleTheme() {
    const next = theme === 'dark' ? 'light' : 'dark';
    setPreference(next);
    try { localStorage.setItem(key, next); } catch { /* The control still works when storage is unavailable. */ }
  }
  return { theme, toggleTheme };
}
