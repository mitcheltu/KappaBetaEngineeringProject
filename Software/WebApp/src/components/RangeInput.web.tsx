import { useId, type CSSProperties } from 'react';

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
  const id = useId();
  const progress = max > min ? ((value - min) / (max - min)) * 100 : 0;
  const valueText = label.toLowerCase().includes('depth')
    ? `${value.toFixed(1)} meters`
    : `${value} beats per minute`;
  const style: CSSProperties & { '--range-progress': string } = {
    '--range-progress': `${Math.min(100, Math.max(0, progress))}%`,
    display: 'block',
    width: '100%',
    height: 48,
    minWidth: 0,
    margin: 0,
  };

  return (
    <input
      id={id}
      type="range"
      className="demo-range"
      aria-label={label}
      aria-valuetext={valueText}
      min={min}
      max={max}
      step={step}
      value={value}
      onChange={(event) => onChange(event.currentTarget.valueAsNumber)}
      style={style}
    />
  );
}
