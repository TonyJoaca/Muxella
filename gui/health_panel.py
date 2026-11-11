"""Read-only health & crackle-debug telemetry; refreshes through the existing IPC worker."""
import json
from datetime import datetime
from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QColor, QBrush
from PyQt6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QLabel, QTableWidget,
    QTableWidgetItem, QPlainTextEdit, QPushButton, QFileDialog, QAbstractItemView
)


def _stream_label(kind: str, index: int) -> str:
    if index == 255 or kind == 'engine':
        return 'Engine'
    if kind == 'input':
        return f'Input {index + 1}'
    return f'A{index + 1}' if index < 5 else f'B{index - 4}'


def _event_stream_label(stage: str, index: int) -> str:
    if index == 255 or stage == 'EngineTimer':
        return 'Engine'
    if stage in ('Capture', 'StripIn', 'StripDSP'):
        return f'Input {index + 1}'
    return f'A{index + 1}' if index < 5 else f'B{index - 4}'


def _format_event(ev: dict) -> str:
    ts_ms = int(ev.get('ts_ms', 0))
    dt = datetime.fromtimestamp(ts_ms / 1000.0) if ts_ms > 0 else datetime.now()
    ts_str = dt.strftime('%H:%M:%S.') + f'{ts_ms % 1000:03d}'
    stage = ev.get('stage', '?')
    idx = int(ev.get('index', 255))
    kind = ev.get('kind', '?')
    val = float(ev.get('value', 0.0))
    stream = _event_stream_label(stage, idx)
    detail = f'{val:.2f}' if kind in ('STEP_JUMP', 'ZERO_DROP', 'LATE_TICK', 'TIMER_RESYNC') else f'{val:.0f}'
    return f'[{ts_str}] {stream:<8} | {stage:<11} | {kind:<20} | val={detail}'


class HealthPanel(QDialog):
    HEADERS = [
        'Stream', 'State', 'Device', 'Format (Buf)', 'Queue ms',
        'Drift PPM', 'Shortfall', 'Dropped', 'Starves', 'Discards',
        'Discont.', 'Glitches', 'Clips', 'RAW'
    ]

    def __init__(self, window):
        super().__init__(window)
        self.window = window
        self.setWindowTitle('Audio health & crackle debugger')
        self.resize(1320, 740)
        self._seen_events = set()
        self._last_snapshot = None

        layout = QVBoxLayout(self)
        self.summary = QLabel('Connecting…')
        layout.addWidget(self.summary)
        self.scheduler = QLabel('Engine scheduler telemetry warming up…')
        layout.addWidget(self.scheduler)
        self.performance = QLabel('Performance counters are warming up…')
        layout.addWidget(self.performance)

        self.table = QTableWidget(16, len(self.HEADERS))
        self.table.setHorizontalHeaderLabels(self.HEADERS)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        layout.addWidget(self.table, stretch=3)

        log_bar = QHBoxLayout()
        log_bar.addWidget(QLabel('Live Crackle & Glitch Event Log (WASAPI discontinuities, starves, discards, timer spikes, waveform drops):'))
        log_bar.addStretch(1)
        self.reset_btn = QPushButton('Reset Counters & Log')
        self.reset_btn.clicked.connect(self._reset_counters)
        log_bar.addWidget(self.reset_btn)
        self.save_btn = QPushButton('Save Debug Report…')
        self.save_btn.clicked.connect(self._save_report)
        log_bar.addWidget(self.save_btn)
        layout.addLayout(log_bar)

        self.event_log = QPlainTextEdit()
        self.event_log.setReadOnly(True)
        self.event_log.setMaximumBlockCount(1000)
        self.event_log.setStyleSheet('font-family: Consolas, monospace; font-size: 8.5pt;')
        self.event_log.setPlaceholderText('No glitch events detected yet. Keep this window open while listening for crackles.')
        layout.addWidget(self.event_log, stretch=2)

        note = QLabel(
            'Queue = mixer ring only (not total end-to-end latency). Format shows Windows device sample rate, channels, bits, and WASAPI buffer frames.\n'
            'Starves = strip input under-blocks or render underrun events. Discards = output backlog trims. Discont. = WASAPI DATA_DISCONTINUITY driver flags.\n'
            'Glitches = signal step jumps (>0.65) or mid-signal zero dropouts (>=8 silent frames after active signal). Cable USB buffers are logged separately by muxella_cables.exe.'
        )
        note.setWordWrap(True)
        layout.addWidget(note)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)

    def showEvent(self, event):
        super().showEvent(event)
        self.timer.start(500)
        self.refresh()

    def hideEvent(self, event):
        self.timer.stop()
        super().hideEvent(event)

    def _reset_counters(self):
        self._seen_events.clear()
        self._last_snapshot = None
        self.event_log.clear()
        self.summary.setText('Counters reset; refreshing…')
        self.window.client.send_command('reset_health')

    def _save_report(self):
        path, _ = QFileDialog.getSaveFileName(self, 'Save Audio Debug Report', 'muxella_crackle_debug.txt', 'Text Files (*.txt);;All Files (*)')
        if not path:
            return
        lines = [
            f'Muxella Audio Health & Crackle Debug Report ({datetime.now().isoformat(timespec="seconds")})',
            self.summary.text(),
            self.scheduler.text(),
            self.performance.text(),
            '',
            '=== Streams ===',
        ]
        if self._last_snapshot:
            for s in self._last_snapshot.get('streams', []):
                label = _stream_label(s.get('kind', ''), int(s.get('index', 0)))
                lines.append(
                    f"{label:<8} active={s.get('active')} dev={s.get('device')} "
                    f"fmt={s.get('device_rate', 0)}Hz/{s.get('device_channels', 0)}ch/{s.get('device_bits', 0)}b({s.get('wasapi_buffer_frames', 0)}f) "
                    f"q={s.get('queue_ms', 0.0):.2f}ms drift={s.get('drift_ppm', 0.0):.1f}ppm "
                    f"shortfall={s.get('shortfall_samples', 0)} dropped={s.get('dropped_samples', 0)} "
                    f"starves={s.get('starves', 0)} discards={s.get('discards', 0)}({s.get('discard_samples', 0)}) "
                    f"discont={s.get('discontinuities', 0)} glitches={s.get('glitches', 0)} clips={s.get('clips', 0)} raw={s.get('raw', 0)}"
                )
        lines.append('')
        lines.append('=== Event Log ===')
        lines.append(self.event_log.toPlainText() or '(none)')
        with open(path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')

    def refresh(self):
        performance = self.window.performance_snapshot
        cpu, ram = performance.get('cpu'), performance.get('ram_bytes')
        cpu_text = f'{cpu:.1f}%' if cpu is not None else '—'
        ram_text = f'{ram / 1048576:.0f} MiB' if ram is not None else '—'
        self.performance.setText(
            f'Combined app CPU: {cpu_text} • RAM working sets: {ram_text}' +
            (' • Partial data (some processes inaccessible)' if performance.get('partial') else '')
        )
        self.performance.setToolTip(self.window.performance_label.toolTip())

        response = self.window.client.send_command('health')
        if not response:
            self.summary.setText('Engine offline / waiting for telemetry')
            return
        try:
            data = json.loads(response)
            streams = data['streams']
        except (ValueError, KeyError, TypeError):
            self.summary.setText('Health telemetry unavailable')
            return

        self._last_snapshot = data
        eng = data.get('engine', {})
        self.scheduler.setText(
            f"Engine timer: {eng.get('total_blocks', 0)} blocks • "
            f"{eng.get('late_ticks', 0)} late ticks • "
            f"{eng.get('timer_resyncs', 0)} resyncs • "
            f"max wake jitter {eng.get('max_jitter_ms', 0.0):.2f} ms • "
            f"max block CPU {eng.get('max_process_ms', 0.0):.2f} ms"
        )

        active = [s for s in streams if s.get('active')]
        shortfall = sum(s.get('shortfall_samples', 0) for s in active)
        drops = sum(s.get('dropped_samples', 0) for s in active)
        starves = sum(s.get('starves', 0) for s in active)
        discards = sum(s.get('discards', 0) for s in active)
        discont = sum(s.get('discontinuities', 0) for s in active)
        glitches = sum(s.get('glitches', 0) for s in active)
        self.summary.setText(
            f"{int(data['sample_rate'])} Hz • {data['block_ms']:.2f} ms mixer block • {len(active)} active streams • "
            f"{shortfall} shortfall • {drops} dropped • {starves} starves • {discards} discards • "
            f"{discont} discont • {glitches} glitches"
        )

        self.table.setRowCount(len(streams))
        warn_bg = QBrush(QColor(90, 24, 34))
        warn_fg = QBrush(QColor(255, 130, 145))
        warn_cols = {6, 7, 8, 9, 10, 11, 12}

        for row, stream in enumerate(streams):
            i = int(stream['index'])
            is_active = bool(stream.get('active'))
            label = _stream_label(stream.get('kind', ''), i)
            state = 'Disabled' if not stream.get('enabled', True) else 'Connected' if is_active else 'Idle'
            if is_active and stream.get('device_rate', 0) > 0:
                fmt = f"{stream['device_rate']}Hz {stream.get('device_channels', 0)}ch {stream.get('device_bits', 0)}b ({stream.get('wasapi_buffer_frames', 0)}f)"
            else:
                fmt = '—'

            drift_val = f"{stream.get('drift_ppm', 0.0):+.1f}" if (is_active and stream.get('kind') == 'input') else '—'
            discard_str = str(stream.get('discards', 0))
            if stream.get('discard_samples', 0) > 0:
                discard_str += f" ({stream['discard_samples']})"

            values = [
                label, state, stream.get('device', 'None'), fmt,
                f"{stream.get('queue_ms', 0.0):.2f}" if is_active else '—',
                drift_val,
                str(stream.get('shortfall_samples', 0)),
                str(stream.get('dropped_samples', 0)),
                str(stream.get('starves', 0)),
                discard_str,
                str(stream.get('discontinuities', 0)),
                str(stream.get('glitches', 0)),
                str(stream.get('clips', 0)),
                {0: 'Not req.', 1: 'Accepted', -1: 'Unavailable'}.get(stream.get('raw', 0), '—') if is_active else '—'
            ]
            for column, value in enumerate(values):
                item = QTableWidgetItem(value)
                if column in warn_cols:
                    numeric = int(value.split()[0])
                    if numeric > 0:
                        item.setBackground(warn_bg)
                        item.setForeground(warn_fg)
                elif column == 3 and is_active and stream.get('device_rate', 0) not in (0, int(data['sample_rate'])):
                    item.setBackground(QBrush(QColor(82, 58, 12)))
                    item.setForeground(QBrush(QColor(255, 220, 120)))
                self.table.setItem(row, column, item)

        self.table.setColumnWidth(2, 230)
        self.table.setColumnWidth(3, 175)

        for ev in data.get('events', []):
            key = (ev.get('ts_ms'), ev.get('stage'), ev.get('index'), ev.get('kind'), round(float(ev.get('value', 0.0)), 3))
            if key not in self._seen_events:
                self._seen_events.add(key)
                self.event_log.appendPlainText(_format_event(ev))
