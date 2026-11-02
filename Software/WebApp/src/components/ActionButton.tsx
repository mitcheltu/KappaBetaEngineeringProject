import { type Ref, type ComponentRef } from 'react';
import { Platform, Pressable, StyleSheet, Text, type View } from 'react-native';
import { colors } from '../theme';
import { Icon, type IconName } from './Icon';

export function ActionButton({ label, onPress, secondary = false, icon, testID, buttonRef }: {
  label: string; onPress: () => void; secondary?: boolean; icon?: IconName; testID?: string; buttonRef?: Ref<ComponentRef<typeof View>>;
}) {
  // dataSet is a React Native Web extension; native builds use the fallback styles.
  const webProps = Platform.OS === 'web'
    ? { dataSet: { variant: secondary ? 'secondary' : 'primary' } }
    : {};

  return <Pressable {...webProps} ref={buttonRef} testID={testID} accessibilityRole="button" accessibilityLabel={label} onPress={onPress}
    style={({ pressed }) => [styles.button, secondary && styles.secondary, pressed && { opacity: 0.8 }]}>
    <Text style={[styles.text, secondary && { color: colors.ocean }]}>{label}</Text>
    {icon && <Icon name={icon} size={18} color={secondary ? colors.sea : colors.ink} />}
  </Pressable>;
}

const styles = StyleSheet.create({
  button: { minHeight: 48, paddingHorizontal: 20, paddingVertical: 11, backgroundColor: colors.action, borderRadius: 0, borderWidth: 1, borderColor: 'transparent', alignItems: 'center', justifyContent: 'center', flexDirection: 'row', gap: 10 },
  secondary: { backgroundColor: 'transparent', borderColor: colors.line },
  text: { fontFamily: Platform.OS === 'web' ? 'Barlow Condensed, sans-serif' : undefined, fontSize: 19, lineHeight: 24, fontWeight: '700', color: colors.ink, flexShrink: 1, textAlign: 'center', letterSpacing: 0.8, textTransform: 'uppercase' },
});
