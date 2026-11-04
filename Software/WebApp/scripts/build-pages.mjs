import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const projectRoot = fileURLToPath(new URL('../', import.meta.url));
const expoCli = fileURLToPath(new URL('../node_modules/expo/bin/cli', import.meta.url));

// An explicitly empty path is valid for a custom domain or user/organization site.
const result = spawnSync(process.execPath, [expoCli, 'export', '--platform', 'web'], {
  cwd: projectRoot,
  stdio: 'inherit',
  env: {
    ...process.env,
    EXPO_BASE_PATH: process.env.EXPO_BASE_PATH ?? '/KappaBetaEngineeringProject',
  },
});

if (result.error) {
  console.error(result.error.message);
}
process.exit(result.status ?? 1);
