import { Platform, Pressable, StyleSheet, Text, View } from 'react-native';

import { colors } from '../theme';
import { Icon } from './Icon';
import RangeInput from './RangeInput';

type DemoValueSliderProps = {
  kind: 'heart' | 'depth';
  value: number;
  onChange: (value: number) => void;
};

const controls = {
  heart: {
    label: 'Resting heart rate',
    min: 20,
    max: 140,
    step: 1,
    initialValue: 70,
    unit: 'bpm',
    category: '01 / BASELINE',
    presets: [60, 70, 90],
  },
  depth: {
    label: 'Estimated depth',
    min: 0,
    max: 10,
    step: 0.1,
    initialValue: 0,
    unit: 'm',
    category: '02 / SIMULATED',
    presets: [0, 2, 5],
  },
} as const;

export function DemoValueSlider({ kind, value, onChange }: DemoValueSliderProps) {
  const control = controls[kind];
  const decimals = kind === 'depth' ? 1 : 0;
  const currentValue = Number.isFinite(value) ? value : control.initialValue;
  const displayedValue = Math.min(control.max, Math.max(control.min, currentValue));

  const updateValue = (nextValue: number) => {
    if (!Number.isFinite(nextValue)) return;

    const clamped = Math.min(control.max, Math.max(control.min, nextValue));
    const snapped =
      control.min + Math.round((clamped - control.min) / control.step) * control.step;
    onChange(Number(snapped.toFixed(decimals)));
  };

  const decreaseDisabled = displayedValue <= control.min;
  const increaseDisabled = displayedValue >= control.max;

  return (
    <View
      nativeID={`${kind}-control`}
      style={[styles.section, kind === 'depth' && styles.lastSection]}
    >
      <View style={styles.header}>
        <View style={styles.description}>
          <View style={styles.categoryRow}>
            <Icon name={kind === 'heart' ? 'heart' : 'depth'} size={15} color={colors.sea} />
            <Text style={styles.category}>{control.category}</Text>
          </View>
          <Text style={styles.label}>{control.label}</Text>
        </View>
        <View style={styles.valueRow}>
          <Text nativeID={`${kind}-value`} style={styles.value}>
            {displayedValue.toFixed(decimals)}
          </Text>
          <Text style={styles.unit}>{control.unit}</Text>
        </View>
      </View>

      <View nativeID={`${kind}-adjustment`} style={styles.controls}>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`Decrease ${control.label.toLowerCase()}`}
          accessibilityState={{ disabled: decreaseDisabled }}
          disabled={decreaseDisabled}
          onPress={() => updateValue(displayedValue - control.step)}
          style={({ pressed }) => [
            styles.stepButton,
            pressed && styles.stepButtonPressed,
            decreaseDisabled && styles.stepButtonDisabled,
          ]}
        >
          <Text style={[styles.stepSymbol, decreaseDisabled && styles.disabledSymbol]}>
            −
          </Text>
        </Pressable>

        <View nativeID={`${kind}-range`} style={styles.range}>
          <RangeInput
            label={control.label}
            value={displayedValue}
            min={control.min}
            max={control.max}
            step={control.step}
            onChange={updateValue}
          />
          <View style={styles.endpoints}>
            <Text style={styles.endpoint}>{control.min} {control.unit}</Text>
            <Text style={styles.endpoint}>{control.max} {control.unit}</Text>
          </View>
        </View>

        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`Increase ${control.label.toLowerCase()}`}
          accessibilityState={{ disabled: increaseDisabled }}
          disabled={increaseDisabled}
          onPress={() => updateValue(displayedValue + control.step)}
          style={({ pressed }) => [
            styles.stepButton,
            pressed && styles.stepButtonPressed,
            increaseDisabled && styles.stepButtonDisabled,
          ]}
        >
          <Text style={[styles.stepSymbol, increaseDisabled && styles.disabledSymbol]}>
            +
          </Text>
        </Pressable>
      </View>

      <View nativeID={`${kind}-presets`} style={styles.presets}>
        {control.presets.map((preset) => {
          const selected = Math.abs(displayedValue - preset) < control.step / 2;
          const presetA11yProps = Platform.OS === 'web'
            ? { 'aria-pressed': selected }
            : { accessibilityState: { selected } };
          const spokenUnit = kind === 'heart'
            ? 'beats per minute'
            : 'meters';

          return (
            <Pressable
              key={preset}
              nativeID={`${kind}-preset-${preset}`}
              {...presetA11yProps}
              accessibilityRole="button"
              accessibilityLabel={`Set ${control.label.toLowerCase()} to ${preset} ${spokenUnit}`}
              onPress={() => updateValue(preset)}
              style={({ pressed }) => [
                styles.presetButton,
                selected && styles.presetSelected,
                pressed && styles.presetPressed,
              ]}
            >
              <Text style={[styles.presetText, selected && styles.presetTextSelected]}>
                {preset} {control.unit}
              </Text>
            </Pressable>
          );
        })}
      </View>
    </View>
  );
}

const monoFont = Platform.select({
  web: "'Space Mono', monospace",
  ios: 'Menlo',
  default: 'monospace',
});

const styles = StyleSheet.create({
  section: {
    paddingVertical: 24,
    borderBottomWidth: 1,
    borderBottomColor: colors.line,
    backgroundColor: 'transparent',
  },
  lastSection: {
    borderBottomWidth: 0,
  },
  header: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 16,
  },
  description: {
    flexBasis: 150,
    flexGrow: 1,
    flexShrink: 1,
    minWidth: 0,
  },
  categoryRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    marginBottom: 8,
  },
  category: {
    flexShrink: 1,
    minWidth: 0,
    color: colors.muted,
    fontSize: 10,
    lineHeight: 16,
    fontFamily: monoFont,
    fontWeight: '400',
    letterSpacing: 1,
  },
  label: {
    color: colors.ocean,
    fontSize: 17,
    fontWeight: '500',
    lineHeight: 25,
  },
  valueRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    maxWidth: '100%',
    alignItems: 'baseline',
    gap: 8,
  },
  value: {
    color: colors.ocean,
    fontSize: 48,
    fontWeight: '400',
    fontFamily: monoFont,
    fontVariant: ['tabular-nums'],
    lineHeight: 58,
    letterSpacing: -2.5,
  },
  unit: {
    color: colors.muted,
    fontSize: 13,
    lineHeight: 22,
    fontFamily: monoFont,
  },
  controls: {
    marginTop: 16,
    flexDirection: 'row',
    alignItems: 'flex-start',
    gap: 12,
  },
  range: {
    flex: 1,
    minWidth: 0,
    minHeight: 48,
    justifyContent: 'center',
  },
  endpoints: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    gap: 8,
    marginTop: 3,
  },
  endpoint: {
    color: colors.muted,
    fontSize: 11,
    lineHeight: 17,
    fontVariant: ['tabular-nums'],
    fontFamily: monoFont,
  },
  stepButton: {
    width: 48,
    height: 48,
    alignItems: 'center',
    justifyContent: 'center',
    borderRadius: 0,
    borderWidth: 1,
    borderColor: colors.line,
    backgroundColor: 'transparent',
  },
  stepButtonPressed: {
    backgroundColor: colors.foam,
  },
  stepButtonDisabled: {
    backgroundColor: 'transparent',
  },
  stepSymbol: {
    color: colors.sea,
    fontSize: 24,
    fontWeight: '400',
    lineHeight: 30,
    fontFamily: monoFont,
  },
  disabledSymbol: {
    color: colors.muted,
    opacity: 0.5,
  },
  presets: {
    marginTop: 16,
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 8,
  },
  presetButton: {
    flexBasis: 64,
    flexGrow: 1,
    minWidth: 48,
    minHeight: 48,
    paddingHorizontal: 8,
    paddingVertical: 10,
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 1,
    borderColor: colors.line,
    borderRadius: 0,
    backgroundColor: 'transparent',
  },
  presetSelected: {
    backgroundColor: colors.action,
  },
  presetPressed: {
    opacity: 0.7,
  },
  presetText: {
    color: colors.muted,
    fontSize: 11,
    lineHeight: 18,
    fontFamily: monoFont,
    fontVariant: ['tabular-nums'],
    textAlign: 'center',
  },
  presetTextSelected: {
    color: colors.ink,
  },
});
