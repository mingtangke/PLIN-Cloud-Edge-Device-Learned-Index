"""Export trained LSTM checkpoints to TorchScript (.pt) for C++ libtorch.

Usage:
  python hot_lstm/export.py --role cloud
  python hot_lstm/export.py --role end --end-id 1
"""
import argparse, os
import torch
from model import LSTMPredictor

SEQ_LEN = 60


def export(model, pt_path: str):
    model.eval()
    scripted = torch.jit.script(model)
    scripted.save(pt_path)
    print(f"[export] saved TorchScript to {pt_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--role', choices=['cloud', 'end'], required=True)
    ap.add_argument('--end-id', type=int, default=1)
    ap.add_argument('--num-devices', type=int, default=10)
    args = ap.parse_args()

    os.makedirs('hot_lstm/models', exist_ok=True)

    if args.role == 'cloud':
        num_dev = args.num_devices
        ckpt_path = 'hot_lstm/models/cloud_lstm.ckpt'
        pt_path   = 'hot_lstm/models/cloud_lstm.pt'
        model = LSTMPredictor(num_devices=num_dev)
    else:
        eid = args.end_id
        edge = 1 if eid <= 5 else 2
        all_ends = list(range(1, 6)) if edge == 1 else list(range(6, 11))
        siblings = [e for e in all_ends if e != eid]
        ckpt_path = f'hot_lstm/models/end_lstm_{eid}.ckpt'
        pt_path   = f'hot_lstm/models/end_lstm_{eid}.pt'
        model = LSTMPredictor(num_devices=len(siblings))

    if os.path.exists(ckpt_path):
        model.load_state_dict(torch.load(ckpt_path, map_location='cpu'))
        print(f"[export] loaded weights from {ckpt_path}")
    else:
        print(f"[export] WARNING: {ckpt_path} not found — exporting untrained model")

    export(model, pt_path)


if __name__ == '__main__':
    main()
