"""Global Windows shortcuts. Registered keys work even while the GUI is hidden."""
import ctypes
from ctypes import wintypes
from PyQt6.QtCore import QAbstractNativeEventFilter, QUrl, Qt
from PyQt6.QtMultimedia import QSoundEffect
from gui.theme import get_resource_dir
from PyQt6.QtGui import QKeySequence
from PyQt6.QtWidgets import (QApplication, QDialog, QVBoxLayout, QLabel, QTableWidget,
                            QTableWidgetItem, QKeySequenceEdit, QPushButton, QMessageBox, QComboBox,
                            QCheckBox, QSlider)

class MSG(ctypes.Structure):
    _fields_ = [('hwnd', wintypes.HWND), ('message', wintypes.UINT),
                ('wParam', wintypes.WPARAM), ('lParam', wintypes.LPARAM),
                ('time', wintypes.DWORD), ('pt', wintypes.POINT)]

class GlobalHotkeys(QAbstractNativeEventFilter):
    def __init__(self, window):
        super().__init__()
        self.window = window
        self.bindings = {}
        self.actions = {}
        self.registered = {}
        self.mic_index = 0
        self.listen_index = 7
        self.deafened = False
        self.mic_was_muted = False
        self._changing_combo = False
        self.feedback_enabled = True
        self.feedback_volume = 15
        self.feedback = QSoundEffect(window)
        self.feedback.setVolume(self.feedback_volume / 100.0)
        self.feedback.setSource(QUrl.fromLocalFile(str(get_resource_dir() / 'sounds' / 'hotkey.wav')))
        self.user32 = ctypes.WinDLL('user32', use_last_error=True)
        self.user32.RegisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT]
        self.user32.UnregisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int]
        for kind, strips in [('input', window.physical_strips + window.virtual_strips), ('bus', window.master_buses)]:
            for index, strip in enumerate(strips):
                label = (f'Input {index + 1}' if kind == 'input' else (f'A{index + 1}' if index < 5 else f'B{index - 4}'))
                for action in ('mute', 'louder', 'quieter'):
                    key = f'{kind}:{index}:{action}'
                    self.actions[key] = (f'{label} — {action}', lambda s=strip, a=action: self.control(s, a))
        self.actions['combo:mute'] = ('Voice combo — toggle microphone mute', self.toggle_voice_mute)
        self.actions['combo:deafen'] = ('Voice combo — toggle deafen (mic + listening strip)', self.toggle_deafen)
        for index, strip in enumerate(window.physical_strips + window.virtual_strips):
            strip.mute_btn.toggled.connect(lambda checked, i=index: self.on_combo_mute_changed(i, checked))
        self.actions['show'] = ('Show / hide Muxella', window._toggle_window_visibility)
        self.actions['restart'] = ('Restart audio engine', window._restart_audio_engine)
        QApplication.instance().installNativeEventFilter(self)

    def combo_strips(self):
        strips = self.window.physical_strips + self.window.virtual_strips
        return strips[self.mic_index], strips[self.listen_index]

    @staticmethod
    def set_muted(strip, muted):
        if strip.mute_btn.isChecked() != muted:
            if strip.mute_btn.isEnabled():
                strip.mute_btn.click()
            else:
                # Keep combo mute state safe if this channel is later powered on.
                strip.mute_btn.setChecked(muted)
                strip._on_mute_clicked(muted)

    def toggle_voice_mute(self):
        self.combo_strips()[0].mute_btn.click()

    def toggle_deafen(self):
        mic, listen = self.combo_strips()
        self._changing_combo = True
        try:
            if self.deafened:
                self.deafened = False
                self.set_muted(listen, False)
                self.set_muted(mic, self.mic_was_muted)
            else:
                self.mic_was_muted = mic.mute_btn.isChecked()
                self.deafened = True
                self.set_muted(mic, True)
                self.set_muted(listen, True)
        finally:
            self._changing_combo = False
        self.window.log_message('Voice combo: ' + ('deafened' if self.deafened else 'listening'))

    def on_combo_mute_changed(self, index, checked):
        if self._changing_combo or not self.deafened or checked:
            return
        if index not in (self.mic_index, self.listen_index):
            return
        mic, listen = self.combo_strips()
        self.deafened = False
        self._changing_combo = True
        try:
            if index == self.mic_index:
                self.set_muted(listen, False)
            else:
                self.set_muted(mic, self.mic_was_muted)
        finally:
            self._changing_combo = False

    def combo_state(self):
        return {'mic': self.mic_index, 'listen': self.listen_index,
                'deafened': self.deafened, 'mic_was_muted': self.mic_was_muted}

    def feedback_state(self):
        return {'enabled': self.feedback_enabled, 'volume': self.feedback_volume}

    def restore_feedback(self, state):
        if not isinstance(state, dict):
            return
        self.feedback_enabled = bool(state.get('enabled', True))
        volume = state.get('volume', 15)
        if isinstance(volume, (int, float)) and 0 <= volume <= 100:
            self.feedback_volume = int(volume)
        self.feedback.setVolume(self.feedback_volume / 100.0)

    def restore_combo(self, state):
        if not isinstance(state, dict):
            return
        mic, listen = state.get('mic', 0), state.get('listen', 7)
        count = len(self.window.physical_strips + self.window.virtual_strips)
        if not isinstance(mic, int) or not isinstance(listen, int) or not (0 <= mic < count and 0 <= listen < count) or mic == listen:
            return
        self.mic_index, self.listen_index = mic, listen
        self.mic_was_muted = bool(state.get('mic_was_muted', False))
        a, b = self.combo_strips()
        self.deafened = bool(state.get('deafened', False)) and a.mute_btn.isChecked() and b.mute_btn.isChecked()

    @staticmethod
    def control(strip, action):
        if action == 'mute':
            strip.mute_btn.click()
        else:
            strip.fader.setValue(strip.fader.value() + (10 if action == 'louder' else -10))

    @staticmethod
    def parse(text):
        sequence = QKeySequence(text)
        if sequence.count() != 1:
            raise ValueError('Use one key combination.')
        combination = sequence[0]
        from PyQt6.QtCore import Qt
        modifiers = combination.keyboardModifiers()
        flags = 0
        for qt, native in [(Qt.KeyboardModifier.AltModifier, 1), (Qt.KeyboardModifier.ControlModifier, 2),
                           (Qt.KeyboardModifier.ShiftModifier, 4), (Qt.KeyboardModifier.MetaModifier, 8)]:
            if modifiers & qt:
                flags |= native
        key = combination.key()
        special = {int(Qt.Key.Key_Space): 32, int(Qt.Key.Key_Up): 38, int(Qt.Key.Key_Down): 40,
                   int(Qt.Key.Key_Left): 37, int(Qt.Key.Key_Right): 39, int(Qt.Key.Key_Insert): 45,
                   int(Qt.Key.Key_Delete): 46, int(Qt.Key.Key_Home): 36, int(Qt.Key.Key_End): 35,
                   int(Qt.Key.Key_PageUp): 33, int(Qt.Key.Key_PageDown): 34}
        if int(Qt.Key.Key_F1) <= key <= int(Qt.Key.Key_F24):
            vk = 112 + key - int(Qt.Key.Key_F1)
        elif 48 <= key <= 90 or key in special:
            vk = special.get(key, key)
        else:
            raise ValueError('Use a letter, number, function key, Space, or navigation key.')
        return flags | 0x4000, vk  # MOD_NOREPEAT

    def clear(self):
        for identifier in self.registered:
            self.user32.UnregisterHotKey(None, identifier)
        self.registered.clear()

    def apply(self, bindings):
        self.clear()
        errors = []
        self.bindings = {key: str(value) for key, value in bindings.items() if key in self.actions and value}
        seen = set()
        for identifier, (key, text) in enumerate(self.bindings.items(), 0x6000):
            try:
                pair = self.parse(text)
                if pair in seen:
                    raise ValueError('Duplicate shortcut.')
                seen.add(pair)
                if not self.user32.RegisterHotKey(None, identifier, *pair):
                    raise ValueError('Already reserved by Windows or another app.')
                self.registered[identifier] = key
            except ValueError as exc:
                errors.append(f'{self.actions[key][0]}: {text}: {exc}')
        return errors

    def nativeEventFilter(self, event_type, message):
        msg = MSG.from_address(int(message))
        if msg.message == 0x0312 and msg.wParam in self.registered:
            self.actions[self.registered[msg.wParam]][1]()
            if self.feedback_enabled and self.feedback_volume and not self.window._is_quitting:
                # Let the fade finish: stopping an audible waveform creates a click.
                # Rapid presses still perform their actions without cutting off the cue.
                if not self.feedback.isPlaying():
                    self.feedback.play()
            return True, 0
        return False, 0

    def open_dialog(self):
        self.clear()  # Editing an existing binding must not activate its audio action.
        dialog = QDialog(self.window)
        dialog.setWindowTitle('Global hotkeys')
        dialog.resize(620, 650)
        layout = QVBoxLayout(dialog)
        layout.addWidget(QLabel('Work while minimized. Volume changes by 1 dB.\nClick a shortcut and press keys; use its clear button to remove it. Single keys are allowed.\nShortcuts are paused while this dialog is open.'))
        sound_enabled = QCheckBox('Quiet sound when a hotkey activates')
        sound_enabled.setChecked(self.feedback_enabled)
        layout.addWidget(sound_enabled)
        volume_label = QLabel(f'Cue volume: {self.feedback_volume}%')
        layout.addWidget(volume_label)
        sound_volume = QSlider(Qt.Orientation.Horizontal)
        sound_volume.setRange(0, 100)
        sound_volume.setValue(self.feedback_volume)
        sound_volume.valueChanged.connect(lambda value: volume_label.setText(f'Cue volume: {value}%'))
        layout.addWidget(sound_volume)
        layout.addWidget(QLabel('Voice combo: deafen mutes both strips. Unmuting the mic also undeafens.\nUndeafen restores the mic’s previous mute state.'))
        mic_selector, listen_selector = QComboBox(), QComboBox()
        for i, strip in enumerate(self.window.physical_strips + self.window.virtual_strips):
            label = f'Input {i + 1}: {strip.title}'
            mic_selector.addItem(label, i)
            listen_selector.addItem(label, i)
        mic_selector.setCurrentIndex(self.mic_index)
        listen_selector.setCurrentIndex(self.listen_index)
        layout.addWidget(QLabel('Microphone strip'))
        layout.addWidget(mic_selector)
        layout.addWidget(QLabel('Listening strip (Discord by default)'))
        layout.addWidget(listen_selector)
        table = QTableWidget(len(self.actions), 2)
        table.setHorizontalHeaderLabels(['Action', 'Shortcut'])
        editors = {}
        for row, (key, (label, _)) in enumerate(self.actions.items()):
            item = QTableWidgetItem(label)
            from PyQt6.QtCore import Qt
            item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
            table.setItem(row, 0, item)
            editor = QKeySequenceEdit(QKeySequence(self.bindings.get(key, '')))
            editor.setMaximumSequenceLength(1)
            editor.setClearButtonEnabled(True)
            table.setCellWidget(row, 1, editor)
            editors[key] = editor
        table.horizontalHeader().setStretchLastSection(True)
        table.setColumnWidth(0, 310)
        layout.addWidget(table)
        save = QPushButton('Apply and save')
        layout.addWidget(save)
        def apply():
            mic, listen = mic_selector.currentData(), listen_selector.currentData()
            if mic == listen:
                QMessageBox.warning(dialog, 'Voice combo', 'Choose two different strips.')
                return
            self.restore_feedback({'enabled': sound_enabled.isChecked(), 'volume': sound_volume.value()})
            if (mic, listen) != (self.mic_index, self.listen_index):
                if self.deafened:
                    self.toggle_deafen()
                self.mic_index, self.listen_index = mic, listen
            bindings = {key: editor.keySequence().toString(QKeySequence.SequenceFormat.PortableText) for key, editor in editors.items()}
            errors = self.apply(bindings)
            self.window.save_settings()
            if errors:
                self.clear()
                QMessageBox.warning(dialog, 'Some shortcuts could not be registered', '\n'.join(errors))
            else:
                dialog.accept()
        save.clicked.connect(apply)
        dialog.exec()
        self.apply(self.bindings)
