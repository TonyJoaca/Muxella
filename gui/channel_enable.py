"""Channel power control shared by input strips and output buses."""
from PyQt6.QtWidgets import QPushButton, QWidget


class ChannelEnableMixin:
    def init_channel_enable(self, layout, output=False):
        self.processing_enabled = True
        self._enable_is_output = output
        self.enable_btn = QPushButton('ENABLED')
        self.enable_btn.setCheckable(True)
        self.enable_btn.setChecked(True)
        self.enable_btn.setFixedHeight(18)
        self.enable_btn.setToolTip('Turn off to stop this channel’s device stream and processing. Settings are retained.')
        self.enable_btn.setStyleSheet('QPushButton {color: #888; border: 1px solid #444; font-size: 7pt;} '
                                     'QPushButton:checked {color: #00dd77; border: 1px solid #176637;}')
        self.enable_btn.clicked.connect(self.set_processing_enabled)
        layout.addWidget(self.enable_btn)

    def set_processing_enabled(self, enabled, reconnect=True):
        enabled = bool(enabled)
        self.processing_enabled = enabled
        self.enable_btn.blockSignals(True)
        self.enable_btn.setChecked(enabled)
        self.enable_btn.blockSignals(False)
        self.enable_btn.setText('ENABLED' if enabled else 'DISABLED')
        index = self.bus_id if self._enable_is_output else self.strip_id
        command = 'bus_enable' if self._enable_is_output else 'strip_enable'
        self.client.send_command(f'{command} {index} {int(enabled)}')
        for child in self.findChildren(QWidget):
            if child is not self.enable_btn:
                child.setEnabled(enabled)
        self._attach_retry_at = 0
        # Clear disconnected styling when off; restored by normal selection checks.
        self.is_offline = True
        self.set_device_connected(True)
        if enabled and reconnect and self.cached_device:
            self.dev_btn.selected_device_id = getattr(self, 'cached_device_id', None)
            self._on_device_selected(self.cached_device)
        self.meter.set_levels(-60.0, -60.0)
