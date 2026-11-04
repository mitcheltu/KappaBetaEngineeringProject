import type { ReactNode } from 'react';
import { Modal, Pressable, StyleSheet, View, useWindowDimensions } from 'react-native';
import { colors } from '../theme';

type SheetFrameProps = {
  open: boolean;
  onClose: () => void;
  children: ReactNode;
};

export default function SheetFrame({ open, onClose, children }: SheetFrameProps) {
  const { width, height } = useWindowDimensions();
  const centered = width >= 700 && height > 500;
  return (
    <Modal
      visible={open}
      transparent
      animationType="none"
      presentationStyle="overFullScreen"
      onRequestClose={onClose}
      statusBarTranslucent
    >
      <View style={[styles.overlay, centered && styles.centered]}>
        <Pressable
          style={StyleSheet.absoluteFill}
          onPress={onClose}
          accessible={false}
          importantForAccessibility="no"
        />
        <View style={styles.sheet} accessibilityViewIsModal>
          {children}
        </View>
      </View>
    </Modal>
  );
}

const styles = StyleSheet.create({
  overlay: {
    flex: 1,
    justifyContent: 'flex-end',
    alignItems: 'center',
    paddingHorizontal: 12,
    paddingBottom: 12,
    backgroundColor: 'rgba(5,26,33,0.75)',
  },
  centered: { justifyContent: 'center', paddingVertical: 24 },
  sheet: {
    width: '100%',
    maxWidth: 480,
    maxHeight: '90%',
    borderRadius: 0,
    backgroundColor: colors.card,
    borderWidth: 1,
    borderColor: colors.line,
    overflow: 'hidden',
  },
});
