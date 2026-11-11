"""Controls Windows playback-session volume through the background IPC worker."""
import time
from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtWidgets import (QDialog, QVBoxLayout, QLabel, QTableWidget,
                            QTableWidgetItem, QSlider, QPushButton, QAbstractItemView)


class AppsPanel(QDialog):
    def __init__(self, window):
        super().__init__(window)
        self.window = window
        self.rows = {}
        self.setWindowTitle('App volumes')
        self.resize(1000, 520)
        layout = QVBoxLayout(self)
        self.status = QLabel('Waiting for Windows audio sessions…')
        layout.addWidget(self.status)
        layout.addWidget(QLabel('Changes the app’s Windows volume. Separate playback sessions appear separately.\nStart playback in an app if it is missing. Exclusive-mode audio may bypass these controls.'))
        self.table = QTableWidget(0, 5)
        self.table.setHorizontalHeaderLabels(['App', 'Playback device', 'Volume', '%', 'Mute'])
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        layout.addWidget(self.table)
        self.timer = QTimer(self)
        self.timer.timeout.connect(lambda: window.client.send_command('apps'))
        window.client.reply_ready.connect(self.on_reply)

    def showEvent(self, event):
        super().showEvent(event)
        self.timer.start(3000)
        self.window.client.send_command('apps')

    def hideEvent(self, event):
        self.timer.stop()
        super().hideEvent(event)

    def on_reply(self, command, data):
        if not self.isVisible() or command not in ('apps', 'app_set'):
            return
        if data.get('status') == 'error':
            self.status.setText(data.get('message', 'App volume unavailable'))
            return
        if command != 'apps':
            return
        sessions = sorted(data.get('sessions', []), key=lambda s: (s['name'].lower(), s['device'], s['id']))
        identifiers = [s['id'] for s in sessions]
        if list(self.rows) != identifiers:
            if any(row['slider'].isSliderDown() for row in self.rows.values()):
                return
            self.rows.clear()
            self.table.setRowCount(len(sessions))
            for row, session in enumerate(sessions):
                identifier = session['id']
                self.table.setItem(row, 0, QTableWidgetItem(session['name']))
                self.table.setItem(row, 1, QTableWidgetItem(session['device']))
                slider = QSlider(Qt.Orientation.Horizontal)
                slider.setRange(0, 100)
                slider.setValue(round(session['volume'] * 100))
                level = QLabel(str(slider.value()))
                mute = QPushButton('Mute')
                mute.setCheckable(True)
                mute.setChecked(session['mute'])
                self.table.setCellWidget(row, 2, slider)
                self.table.setCellWidget(row, 3, level)
                self.table.setCellWidget(row, 4, mute)
                self.rows[identifier] = {'slider': slider, 'mute': mute, 'level': level, 'pending_until': 0}
                slider.valueChanged.connect(lambda _, key=identifier: self.send(key))
                mute.clicked.connect(lambda _, key=identifier: self.send(key))
            self.table.setColumnWidth(0, 180)
            self.table.setColumnWidth(1, 310)
            self.table.setColumnWidth(2, 280)
        else:
            for session in sessions:
                row = self.rows[session['id']]
                if row['slider'].isSliderDown() or time.monotonic() < row['pending_until']:
                    continue
                row['slider'].blockSignals(True)
                row['slider'].setValue(round(session['volume'] * 100))
                row['slider'].blockSignals(False)
                row['level'].setText(str(row['slider'].value()))
                row['mute'].setChecked(session['mute'])
        self.status.setText(f'{len(sessions)} playback sessions • Updates every 3 seconds')

    def send(self, identifier):
        row = self.rows.get(identifier)
        if row is None:
            return
        row['pending_until'] = time.monotonic() + 2
        row['level'].setText(str(row['slider'].value()))
        self.window.client.send_command(f"app_set {identifier} {row['slider'].value() / 100:.2f} {int(row['mute'].isChecked())}")
