import Svg, { Circle, Defs, Ellipse, LinearGradient, Path, Rect, Stop } from 'react-native-svg';

/** Decorative concept illustration, not a depiction of a connected physical device. */
export function WearableVisual({ connected }: { connected: boolean }) {
  return <Svg width="100%" height="100%" viewBox="0 0 260 180" fill="none" aria-hidden>
    <Defs>
      <LinearGradient id="band" x1="100" y1="12" x2="160" y2="170" gradientUnits="userSpaceOnUse"><Stop stopColor="#274E63" /><Stop offset="0.45" stopColor="#0A2536" /><Stop offset="1" stopColor="#163D50" /></LinearGradient>
      <LinearGradient id="device" x1="90" y1="50" x2="165" y2="128" gradientUnits="userSpaceOnUse"><Stop stopColor="#345E70" /><Stop offset="0.5" stopColor="#163C50" /><Stop offset="1" stopColor="#0A2A3C" /></LinearGradient>
      <LinearGradient id="ring" x1="50" y1="20" x2="210" y2="160" gradientUnits="userSpaceOnUse"><Stop stopColor="#85E5DF" stopOpacity="0.25" /><Stop offset="0.6" stopColor="#8AD8E5" stopOpacity="0.04" /><Stop offset="1" stopColor="#85E5DF" stopOpacity="0.2" /></LinearGradient>
    </Defs>
    <Ellipse cx="130" cy="91" rx="111" ry="77" stroke="url(#ring)" />
    <Ellipse cx="130" cy="91" rx="88" ry="62" stroke="url(#ring)" strokeDasharray="3 8" />
    <Ellipse cx="130" cy="94" rx="66" ry="48" stroke="#66B6C3" strokeOpacity="0.12" />
    <Circle cx="30" cy="58" r="3" fill="#76DED7" fillOpacity="0.6" />
    <Circle cx="230" cy="122" r="2" fill="#9FCBDA" fillOpacity="0.7" />
    <Rect x="109" y="13" width="44" height="153" rx="19" fill="url(#band)" stroke="#446B7B" strokeOpacity="0.6" />
    <Path d="M118 24h26M118 31h26M118 149h26M118 156h26" stroke="#8DB7C8" strokeOpacity="0.13" />
    <Rect x="91" y="55" width="79" height="73" rx="25" fill="#041B29" stroke="#6B95A5" strokeOpacity="0.4" />
    <Rect x="94" y="56" width="73" height="67" rx="23" fill="url(#device)" stroke="#95C5D1" strokeOpacity="0.3" />
    <Path d="M108 88c7-10 14 10 21 0s14 10 21 0M108 98c7-10 14 10 21 0s14 10 21 0" stroke="#93E8E2" strokeWidth="2" strokeLinecap="round" />
    <Circle cx="145" cy="73" r="2.2" fill={connected ? '#91FFCA' : '#B6D2DC'} />
  </Svg>;
}
