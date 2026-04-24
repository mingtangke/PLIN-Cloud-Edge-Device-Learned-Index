"""Shared LSTM model class for Cloud (num_devices=10) and End (num_devices=4)."""
import torch
import torch.nn as nn


class LSTMPredictor(nn.Module):
    def __init__(self, num_devices: int, hidden_size: int = 64,
                 num_layers: int = 2, seq_len: int = 60):
        super().__init__()
        self.num_devices = num_devices
        self.seq_len = seq_len
        self.lstm = nn.LSTM(num_devices, hidden_size, num_layers, batch_first=True)
        self.fc   = nn.Linear(hidden_size, num_devices)

    def forward(self, x):
        # x: [B, seq_len, num_devices]
        out, _ = self.lstm(x)
        return self.fc(out[:, -1, :])   # [B, num_devices]

    @torch.jit.export
    def predict_top_k(self, x: torch.Tensor, k: int) -> torch.Tensor:
        """Returns top-k device indices (sorted by probability, desc)."""
        logits = self.forward(x)
        probs  = torch.softmax(logits, dim=-1)
        _, indices = torch.topk(probs, k, dim=-1)
        return indices  # [B, k]
