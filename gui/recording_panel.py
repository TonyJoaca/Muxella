"""Select mixer tracks and control the engine's background WAV writer."""
import json
from datetime import datetime
from pathlib import Path
from PyQt6.QtWidgets import (QDialog, QVBoxLayout, QHBoxLayout, QGridLayout, QLabel,
                            QCheckBox, QPushButton, QLineEdit, QFileDialog, QGroupBox, QPlainTextEdit)


class RecordingPanel(QDialog):
    def __init__(self, window):
        super().__init__(window)
        self.window = window
        self.pending = False
        self.choices = []
        self.setWindowTitle('WAV recording')
        self.resize(680, 580)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel('One stereo 32-bit float WAV per selected track, at the engine sample rate.\nRecords after strip/bus processing and mute. Closing this panel keeps recording.'))
        preferences = window.recording_preferences
        self.folder = QLineEdit(preferences.get('folder', str(Path.home() / 'Music' / 'Muxella Recordings')))
        folder_row = QHBoxLayout()
        folder_row.addWidget(self.folder)
        self.browse_button = QPushButton('Choose folder…')
        self.browse_button.clicked.connect(self.browse)
        folder_row.addWidget(self.browse_button)
        layout.addLayout(folder_row)
        selected = preferences.get('sources', [{'kind': 'bus', 'index': 5}])
        for kind, strips in [('input', window.physical_strips + window.virtual_strips), ('bus', window.master_buses)]:
            group = QGroupBox('Input strips' if kind == 'input' else 'Output buses')
            grid = QGridLayout(group)
            for index, strip in enumerate(strips):
                label = f'Input {index + 1}: {strip.title}' if kind == 'input' else f'{strip.bus_code}: {strip.name}'
                check = QCheckBox(label)
                source = {'kind': kind, 'index': index}
                check.setChecked(source in selected)
                grid.addWidget(check, index // 2, index % 2)
                self.choices.append((check, source))
            layout.addWidget(group)
        self.status = QLabel('Idle')
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        self.files = QPlainTextEdit()
        self.files.setReadOnly(True)
        self.files.setMaximumHeight(100)
        layout.addWidget(self.files)
        buttons = QHBoxLayout()
        self.start_button = QPushButton('Start recording')
        self.stop_button = QPushButton('Stop and save WAV files')
        self.stop_button.setEnabled(False)
        self.start_button.clicked.connect(self.start)
        self.stop_button.clicked.connect(self.stop)
        buttons.addWidget(self.start_button)
        buttons.addWidget(self.stop_button)
        layout.addLayout(buttons)
        window.client.reply_ready.connect(self.on_reply)

    def showEvent(self, event):
        super().showEvent(event)
        self.window.client.send_command('record_status')

    def browse(self):
        folder = QFileDialog.getExistingDirectory(self, 'Recording folder', self.folder.text())
        if folder:
            self.folder.setText(folder)

    def start(self):
        sources = [source for check, source in self.choices if check.isChecked()]
        if not sources:
            self.status.setText('Select at least one strip or bus.')
            return
        if not self.folder.text().strip():
            self.status.setText('Choose a recording folder.')
            return
        root = Path(self.folder.text()).expanduser().absolute()
        self.window.recording_preferences = {'folder': str(root), 'sources': sources}
        self.window.save_settings()
        folder = root / datetime.now().strftime('%Y-%m-%d_%H-%M-%S-%f')
        self.pending = True
        self.start_button.setEnabled(False)
        self.stop_button.setEnabled(False)
        self.status.setText('Starting recording…')
        self.window.client.send_command('record_start ' + json.dumps({'folder': str(folder), 'sources': sources}, ensure_ascii=True))

    def stop(self):
        self.pending = True
        self.start_button.setEnabled(False)
        self.stop_button.setEnabled(False)
        self.status.setText('Finishing WAV files…')
        self.window.client.send_command('record_stop')

    def on_reply(self, command, data):
        if command not in ('record_start', 'record_stop', 'record_status'):
            return
        if command == 'record_status' and self.pending:
            return
        self.pending = False
        if data.get('status') == 'error':
            self.status.setText(data.get('message', 'Recording unavailable'))
            self.start_button.setEnabled(False)
            self.stop_button.setEnabled(True)
            return
        active = data.get('active', False)
        self.start_button.setEnabled(not active)
        self.stop_button.setEnabled(active or bool(data.get('error')))
        self.folder.setEnabled(not active)
        self.browse_button.setEnabled(not active)
        for check, _ in self.choices:
            check.setEnabled(not active)
        seconds = int(data.get('seconds', 0))
        duration = f'{seconds // 3600:02}:{seconds // 60 % 60:02}:{seconds % 60:02}'
        drops = data.get('dropped_frames', 0)
        message = data.get('error') or ('Recording' if active else 'Stopped / WAV files saved' if data.get('files') else 'Idle')
        self.status.setText(f'{message} • {duration} • {drops} dropped frames (silence inserted)')
        self.files.setPlainText('\n'.join(data.get('files', [])))
