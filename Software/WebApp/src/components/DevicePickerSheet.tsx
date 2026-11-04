import { useEffect, useRef, type ComponentRef } from 'react';
import { ActivityIndicator, Platform, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { type ConnectionSnapshot, type MockConnectionService } from '../services/mockConnection';
import { colors } from '../theme';
import { ActionButton } from './ActionButton';
import { Icon } from './Icon';
import SheetFrame from './SheetFrame';

export function DevicePickerSheet({ snapshot, service }: { snapshot: ConnectionSnapshot; service: MockConnectionService }) {
  const { status, deviceName } = snapshot;
  const closeRef = useRef<ComponentRef<typeof View>>(null);
  useEffect(() => {
    if (status === 'connecting') closeRef.current?.focus();
  }, [status]);
  const open = ['scanning', 'results', 'connecting', 'empty', 'error'].includes(status);
  const pending = status === 'scanning' || status === 'connecting';
  const heading = status === 'scanning' ? 'Find a device' : status === 'connecting' ? 'Connecting' : status === 'empty' ? 'Nothing found' : status === 'error' ? 'Connection failed' : 'Select device';
  return <SheetFrame open={open} onClose={service.close}>
    <ScrollView style={{ maxHeight: '100%' }} contentContainerStyle={styles.content}>
      <View style={styles.top}>
        <Text style={styles.eyebrow}>SIMULATED BLUETOOTH</Text>
        <Pressable ref={closeRef} accessibilityRole="button" accessibilityLabel="Close device picker" onPress={service.close} style={({ pressed }) => [styles.close, pressed && { opacity: 0.7 }]}><Icon name="close" size={19} color={colors.muted} /></Pressable>
      </View>
      <Text accessibilityRole="header" accessibilityLiveRegion="polite" style={styles.heading}>{heading}</Text>
      <Text style={styles.description}>Demo only. No hardware connection.</Text>

      <View accessibilityLiveRegion="polite" style={styles.body}>
        {pending && <View nativeID="picker-progress" style={styles.pending}>
          <ActivityIndicator color={colors.sea} size="small" accessibilityLabel="Mock connection in progress" />
          <Text style={styles.bodyText}>{status === 'scanning' ? 'Searching…' : deviceName}</Text>
        </View>}
        {status === 'results' && <>
          <Pressable nativeID="demo-device-option" accessibilityRole="button" accessibilityLabel={`Select ${deviceName}`} onPress={service.connect} style={({ pressed }) => [styles.device, pressed && { opacity: 0.75 }]}>
            <View style={styles.deviceIcon}><Icon name="watch" size={25} color={colors.sea} /></View>
            <View style={{ flex: 1, minWidth: 0 }}><Text style={styles.deviceName}>{deviceName}</Text><Text style={styles.deviceCaption}>Simulated device</Text></View>
            <View nativeID="demo-device-arrow"><Icon name="arrow" size={20} color={colors.sea} /></View>
          </Pressable>
        </>}
        {status === 'empty' && <>
          <Text style={styles.bodyText}>Retry, or change the scenario.</Text>
        </>}
        {status === 'error' && <>
          <Text style={styles.bodyText}>Retry, or change the scenario.</Text>
        </>}
      </View>

      <View style={{ gap: 10 }}>
        {status === 'empty' && <ActionButton label="Try again" onPress={service.scan} icon="reset" />}
        {status === 'error' && <><ActionButton label="Retry connection" onPress={service.connect} icon="reset" /><ActionButton label="Choose device" onPress={service.chooseDevice} secondary /></>}
        <ActionButton label={pending ? 'Cancel' : 'Close'} onPress={service.close} secondary />
      </View>
    </ScrollView>
  </SheetFrame>;
}

const styles = StyleSheet.create({
  content: { paddingHorizontal: 24, paddingTop: 12, paddingBottom: 24 },
  top: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 12, marginBottom: 8 },
  eyebrow: { fontSize: 10, lineHeight: 16, letterSpacing: 1.5, fontWeight: '700', color: colors.sea, flexShrink: 1 },
  close: { width: 48, height: 48, alignItems: 'center', justifyContent: 'center', borderRadius: 0, backgroundColor: 'transparent', borderWidth: 1, borderColor: colors.line, flexShrink: 0 },
  heading: { fontFamily: Platform.OS === 'web' ? 'Barlow Condensed, sans-serif' : undefined, fontSize: 38, lineHeight: 42, fontWeight: '700', color: colors.ocean, letterSpacing: 0.2, textTransform: 'uppercase' },
  description: { fontSize: 13, lineHeight: 21, color: colors.muted, marginTop: 8 },
  body: { paddingVertical: 24, gap: 12 },
  pending: { flexDirection: 'row', alignItems: 'center', gap: 12, paddingVertical: 8, borderRadius: 0 },
  bodyText: { color: colors.muted, fontSize: 14, lineHeight: 23, textAlign: 'left', flexShrink: 1 },
  device: { minHeight: 72, flexDirection: 'row', alignItems: 'center', gap: 12, padding: 16, backgroundColor: 'transparent', borderRadius: 0, borderWidth: 1, borderColor: colors.line },
  deviceIcon: { width: 32, height: 48, borderRadius: 0, alignItems: 'center', justifyContent: 'center', flexShrink: 0 },
  deviceName: { fontSize: 15, lineHeight: 23, fontWeight: '600', color: colors.ocean, flexShrink: 1 },
  deviceCaption: { fontSize: 12, lineHeight: 19, color: colors.muted, marginTop: 4 },
});
