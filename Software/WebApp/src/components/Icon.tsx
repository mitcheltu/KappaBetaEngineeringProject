import Svg, { Circle, Path, Rect } from 'react-native-svg';
import { colors } from '../theme';

export type IconName = 'wave' | 'bluetooth' | 'close' | 'arrow' | 'check' | 'heart' | 'depth' | 'motion' | 'reset' | 'chevron' | 'watch';

export function Icon({ name, size = 24, color = colors.ocean }: { name: IconName; size?: number; color?: string }) {
  const paths: Partial<Record<IconName, string>> = {
    wave: 'M2 10c3.3-6 6.7 6 10 0s6.7 6 10 0M2 16c3.3-6 6.7 6 10 0s6.7 6 10 0',
    bluetooth: 'M7 7l10 10-5 4V3l5 4L7 17',
    close: 'M6 6l12 12M18 6L6 18',
    arrow: 'M5 12h14m-5-5 5 5-5 5',
    check: 'm5 12 4 4L19 6',
    heart: 'M20.8 4.6a5.5 5.5 0 0 0-7.8 0L12 5.7l-1.1-1.1a5.5 5.5 0 0 0-7.8 7.8L12 21l8.8-8.6a5.5 5.5 0 0 0 0-7.8Z',
    depth: 'M12 3v16m-5-5 5 5 5-5M4 4h3m10 0h3M4 21h16',
    motion: 'M2 12h5l3-8 4 16 3-8h5',
    reset: 'M3 11a9 9 0 1 1 2.6 7M3 4v7h7',
    chevron: 'm6 9 6 6 6-6',
  };
  return (
    <Svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth={1.65} strokeLinecap="round" strokeLinejoin="round" aria-hidden>
      {name === 'watch' ? <><Path d="m8 6 1-4h6l1 4m-8 12 1 4h6l1-4" /><Rect x={5} y={6} width={14} height={12} rx={4} /><Circle cx={12} cy={12} r={2} /></> : <Path d={paths[name]} />}
    </Svg>
  );
}
