// Expo replaces EXPO_BASE_URL with experiments.baseUrl in the web bundle.
// Keep the stylesheet and its relative font URLs within the deployment path.
if (typeof document !== 'undefined' && !document.getElementById('local-fonts')) {
  const stylesheet = document.createElement('link');
  stylesheet.id = 'local-fonts';
  stylesheet.rel = 'stylesheet';
  stylesheet.href = `${process.env.EXPO_BASE_URL ?? ''}/fonts/fonts.css`;
  document.head.appendChild(stylesheet);
}

export {};
