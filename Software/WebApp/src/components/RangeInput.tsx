import Slider from '@react-native-community/slider';

import { colors } from '../theme';

type RangeInputProps = {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onChange: (value: number) => void;
};

export default function RangeInput({
  label,
  value,
  min,
  max,
  step,
  onChange,
}: RangeInputProps) {
  const valueText = label.toLowerCase().includes('depth')
    ? `${value.toFixed(1)} meters`
    : `${value} beats per minute`;

  return (
    <Slider
      accessibilityLabel={label}
      accessibilityValue={{ min, max, now: value, text: valueText }}
      minimumValue={min}
      maximumValue={max}
      step={step}
      value={value}
      onValueChange={onChange}
      minimumTrackTintColor={colors.sea}
      maximumTrackTintColor={colors.line}
      thumbTintColor={colors.sea}
      style={{ width: '100%', minWidth: 0, height: 48 }}
    />
  );
}
