// Keep local development at /; Pages supplies its actual path at build time.
module.exports = ({ config }) => {
  const basePath = (process.env.EXPO_BASE_PATH ?? '').replace(/\/+$/, '');

  if (basePath && !/^\/[A-Za-z0-9._/-]+$/.test(basePath)) {
    throw new Error('EXPO_BASE_PATH must be a URL path such as /KappaBetaEngineeringProject, or empty for the site root.');
  }

  return {
    ...config,
    experiments: {
      ...config.experiments,
      baseUrl: basePath,
    },
  };
};
