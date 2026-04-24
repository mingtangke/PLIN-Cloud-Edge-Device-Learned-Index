# hotkey_predictor.py
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader
import pandas as pd
import numpy as np
import socket
import threading
import time
from collections import Counter, defaultdict
import select
import os
import re
import warnings
from torch.cuda.amp import autocast, GradScaler
from typing import List, Tuple, Dict, Any, Optional

warnings.filterwarnings('ignore')

class KeySequenceDataset(Dataset):
    """Dataset for key sequence prediction with full sequences"""
    
    def __init__(self, sequences, key_to_idx, window_size, top_k_hot_keys, target_windows_size):
        self.sequences = sequences
        self.key_to_idx = key_to_idx
        self.window_size = window_size
        self.top_k_hot_keys = top_k_hot_keys
        self.vocab_size = len(key_to_idx)
        self.target_windows_size = target_windows_size
    
    def __len__(self):
        return len(self.sequences)
    
    def __getitem__(self, idx):
        windows = self.sequences[idx] if idx < len(self.sequences) else []
        next_windows = self.sequences[idx + 1] if idx + 1 < len(self.sequences) else []

        input_windows = windows 
        target_windows = next_windows[-self.target_windows_size:]  
        
        seq_indices = []
        for window_keys in input_windows:
            indices = [self.key_to_idx.get(key, 0) for key in window_keys]
            if len(indices) < self.window_size:
                indices = indices + [0] * (self.window_size - len(indices))
            else:
                indices = indices[:self.window_size]
            seq_indices.append(indices)
        
        target_keys_flattened = []
        for window in target_windows:
            target_keys_flattened.extend(window)
        
        target_key_counter = Counter(target_keys_flattened)
        target_hot_keys = [key for key, _ in target_key_counter.most_common(self.top_k_hot_keys)]
        
        target_indices = [self.key_to_idx.get(key, 0) for key in target_hot_keys]
        target_padded = target_indices + [0] * max(0, self.top_k_hot_keys - len(target_indices))
        target_padded = target_padded[:self.top_k_hot_keys]
        
        return torch.tensor(seq_indices, dtype=torch.long), torch.tensor(target_padded, dtype=torch.long)

class EfficientKeyPredictionLSTM(nn.Module):
    
    def __init__(self, vocab_size, window_size, embedding_dim=32, hidden_size=64, num_layers=2, dropout_rate=0.5):
        super().__init__()
        self.vocab_size = vocab_size
        self.window_size = window_size
        self.embedding = nn.Embedding(vocab_size + 1, embedding_dim, padding_idx=0)
        self.lstm = nn.LSTM(
            input_size=embedding_dim * window_size,
            hidden_size=hidden_size,
            num_layers=num_layers,  
            dropout=dropout_rate,
            batch_first=True
        )
        self.fc = nn.Linear(hidden_size, vocab_size)

    def forward(self, x):
        batch, seq_len, win_size = x.shape
        
        x = x.view(batch * seq_len, win_size)
        emb = self.embedding(x)
        emb = emb.view(batch, seq_len, -1)
        lstm_out, _ = self.lstm(emb)
        last_out = lstm_out[:, -1, :]

        return self.fc(last_out)

class KeyAccuracyMetrics:
    
    def __init__(self, k_list=[5000,10000]):
        self.k_list = k_list
        
    def __call__(self, predictions, targets):
        batch_size, vocab_size = predictions.shape
        results = {}
        
        for k in self.k_list:
            _, pred_indices = predictions.topk(k, dim=-1)
            
            total_recall = 0.0
            total_precision = 0.0
            total_f1 = 0.0
            total_accuracy = 0.0
            total_samples = 0
            
            for i in range(batch_size):
                non_zero_targets = targets[i][targets[i] != 0]
                if len(non_zero_targets) == 0:
                    continue
                    
                target_set = set(non_zero_targets.tolist())
                pred_set = set(pred_indices[i].tolist())
                
                intersection = pred_set & target_set
                tp = len(intersection)
            
                recall = tp / len(target_set) if len(target_set) > 0 else 0.0
                precision = tp / k if k > 0 else 0.0
                f1 = 2 * (precision * recall) / (precision + recall) if (precision + recall) > 0 else 0.0
                accuracy = tp / min(len(target_set), k) if min(len(target_set), k) > 0 else 0.0
                
                total_recall += recall
                total_precision += precision
                total_f1 += f1
                total_accuracy += accuracy
                total_samples += 1
            
            if total_samples > 0:
                results[f'top_{k}_recall'] = total_recall / total_samples
                results[f'top_{k}_precision'] = total_precision / total_samples
                results[f'top_{k}_f1'] = total_f1 / total_samples
                results[f'top_{k}_accuracy'] = total_accuracy / total_samples
            else:
                for metric in ['recall', 'precision', 'f1', 'accuracy']:
                    results[f'top_{k}_{metric}'] = 0.0
        
        return results

class HotKeyPredictor:
    """Main class for hot key prediction with communication capabilities"""
    
    def __init__(self, window_size=500, sequence_length=300, top_k_hot_keys=10000, prediction_win=300):
        self.window_size = window_size
        self.sequence_length = sequence_length
        self.top_k_hot_keys = top_k_hot_keys
        self.key_to_idx = {}
        self.idx_to_key = {}
        self.model = None
        self.prediction_win = prediction_win
        self.device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
        print(f"Using device: {self.device}")
        
        # For data statistics
        self.window_stats = {}
        
        # Communication setup
        self.sock = None
        self.conn = None
        self.addr = None
        self.running = False
        
        # Log file path
        self.log_file = "/home/ming/桌面/PLIN-N /PLIN-N/build/key_log.csv"
        
    def setup_communication(self, host='127.0.0.1', port=60001):
        """Set up socket communication with C++"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(1)
        print(f"Listening for C++ connection on {host}:{port}")
        
        self.conn, self.addr = self.sock.accept()
        print(f"Connected to C++ at {self.addr}")

        
    def listen_for_messages(self):
        """Listen for messages from C++"""
        self.running = True
        
        while self.running:
            try:
                # Check if data is available
                ready = select.select([self.conn], [], [], 0.1)
                if ready[0]:
                    data = self.conn.recv(1024).decode('utf-8')
                    
                    if not data:
                        continue
                        
                    if data.startswith("INDEX:"):
                        # Parse index range
                        parts = data.split(":")
                        if len(parts) >= 3:
                            start_idx = int(parts[1])
                            end_idx = int(parts[2])
                            
                            # Process new data
                            self.train_and_predict(start_idx, end_idx)
                        
            except Exception as e:
                print(f"Error in communication: {e}")
                time.sleep(1)

    def preprocess_data(self, df: pd.DataFrame) -> List:
        print("Starting data preprocessing...")
        start_time = time.time()

        windows = []
        total_records = len(df)
        window_step = max(1, self.window_size)
        
        print(f"Total records: {total_records}")
        print(f"Window size: {self.window_size} keys, Step: {window_step} keys")
        
        for i in range(0, total_records - self.window_size + 1, window_step):
            window_keys = df['key'].iloc[i:i+self.window_size].tolist()
            windows.append(window_keys)
        
        print(f"Created {len(windows)} windows")
        
        # Create key vocabulary from all keys
        all_keys = set()
        for key_list in windows:
            all_keys.update(key_list)
        
        # Create vocabulary mapping
        sorted_keys = sorted(all_keys)
        self.key_to_idx = {key: idx+1 for idx, key in enumerate(sorted_keys)}
        self.key_to_idx["[PAD]"] = 0
        self.idx_to_key = {idx: key for key, idx in self.key_to_idx.items()}
        
        keys_per_window = [len(set(key_list)) for key_list in windows]
        self.window_stats = {
            'total_unique_keys': len(all_keys),
            'total_windows': len(windows),
            'avg_unique_keys_per_window': np.mean(keys_per_window),
            'max_unique_keys_per_window': np.max(keys_per_window),
            'min_unique_keys_per_window': np.min(keys_per_window),
            'window_size': self.window_size,
            'top_k_hot_keys': self.top_k_hot_keys
        }
        
        print(f"\nData Statistics:")
        print(f"  Total unique keys: {self.window_stats['total_unique_keys']}")
        print(f"  Total windows: {self.window_stats['total_windows']}")
        print(f"  Avg unique keys per window: {self.window_stats['avg_unique_keys_per_window']:.2f}")
        print(f"  Max unique keys per window: {self.window_stats['max_unique_keys_per_window']}")
        print(f"  Min unique keys per window: {self.window_stats['min_unique_keys_per_window']}")
        
        sequences = []
        
        print(f"\nCreating training sequences with {self.sequence_length} historical windows...")
     
        for i in range(len(windows) - self.sequence_length + 1):
            seq = windows[i:i + self.sequence_length]
            sequences.append(seq)
        
        print(f"Created {len(sequences)} training sequences in {time.time()-start_time:.2f} seconds")
        
        return sequences
    
    def create_data_loaders(self, sequences, batch_size=16, test_size=0.2, val_size=0.1):
        print("\nCreating data loaders...")
        start_time = time.time()
        total_samples = len(sequences)
        train_end = int(total_samples * (1 - test_size - val_size))
        val_end = int(total_samples * (1 - test_size))
        
        train_seq = sequences[:train_end]
        val_seq = sequences[train_end:val_end]
        test_seq = sequences[val_end:]
        
        print(f"  Train samples: {len(train_seq)}")
        print(f"  Validation samples: {len(val_seq)}")
        print(f"  Test samples: {len(test_seq)}")
        
        train_dataset = KeySequenceDataset(train_seq, self.key_to_idx, self.window_size, self.top_k_hot_keys, self.prediction_win)
        val_dataset = KeySequenceDataset(val_seq, self.key_to_idx, self.window_size, self.top_k_hot_keys, self.prediction_win)
        test_dataset = KeySequenceDataset(test_seq, self.key_to_idx, self.window_size, self.top_k_hot_keys, self.prediction_win)
 
        train_loader = DataLoader(train_dataset, batch_size=batch_size, shuffle=True, num_workers=4, pin_memory=True)
        val_loader = DataLoader(val_dataset, batch_size=batch_size, shuffle=False, num_workers=4, pin_memory=True)
        test_loader = DataLoader(test_dataset, batch_size=batch_size, shuffle=False, num_workers=4, pin_memory=True)
        
        print(f"Data loaders created in {time.time()-start_time:.2f} seconds")
        return train_loader, val_loader, test_loader
    
    def set_based_loss(self, predictions, targets, base_emphasis=1.0, max_emphasis=3.0):
        """Adaptive heat-enhanced loss"""
        batch_size, vocab_size = predictions.shape
        
        # Create multi-hot encoding and count actual hot keys
        target_multihot = torch.zeros(batch_size, vocab_size, 
                                    device=predictions.device, 
                                    dtype=torch.float)
        actual_key_counts = torch.zeros(batch_size, device=predictions.device)
        
        for i in range(batch_size):
            actual_keys = targets[i][targets[i] != 0]
            if len(actual_keys) > 0:
                target_multihot[i, actual_keys] = 1.0
                actual_key_counts[i] = len(actual_keys)
        
        # Calculate adaptive emphasis factor
        avg_key_count = actual_key_counts.float().mean()
        emphasis_factor = base_emphasis + (max_emphasis - base_emphasis) * torch.sigmoid(
            - (avg_key_count - 5) / 2
        )
        
        # Calculate sigmoid probability
        pred_prob = torch.sigmoid(predictions)
        
        # Base focal loss
        alpha = 0.25
        gamma = 2.0
        bce_loss = nn.BCELoss(reduction='none')(pred_prob, target_multihot)
        p_t = target_multihot * pred_prob + (1 - target_multihot) * (1 - pred_prob)
        modulating_factor = (1 - p_t) ** gamma
        alpha_factor = target_multihot * alpha + (1 - target_multihot) * (1 - alpha)
        focal_loss = alpha_factor * modulating_factor * bce_loss
        
        # Heat enhancement term
        heat_enhancement = target_multihot * (1 - pred_prob) ** emphasis_factor
        
        # Combined loss
        total_loss = focal_loss + heat_enhancement
        
        return total_loss.mean()
  
    def train_model(self, train_loader, val_loader, epochs=1, lr=0.001, patience=3):
        """Train the LSTM model"""
        vocab_size = len(self.key_to_idx)
        print(f"\nInitializing efficient model with vocabulary size: {vocab_size}")
        start_time = time.time()
        
        self.model = EfficientKeyPredictionLSTM(
            vocab_size=vocab_size,
            window_size=self.window_size,
            embedding_dim=16,
            hidden_size=16,
            num_layers=1,
            dropout_rate=0.3
        ).to(self.device)
        
        total_params = sum(p.numel() for p in self.model.parameters())
        trainable_params = sum(p.numel() for p in self.model.parameters() if p.requires_grad)
        print(f"Model parameters: {total_params:,} total, {trainable_params:,} trainable")
      
        optimizer = optim.AdamW(self.model.parameters(), lr=lr, weight_decay=1e-4)
        scheduler = optim.lr_scheduler.ReduceLROnPlateau(
            optimizer, mode='min', patience=patience//2, factor=0.5, verbose=True
        )
        
        train_losses = []
        val_losses = []
        val_metrics_history = []
        best_val_loss = float('inf')
        patience_counter = 0
     
        metrics_calculator = KeyAccuracyMetrics(k_list=[self.top_k_hot_keys])
        
        print(f"\nStarting training for {epochs} epochs...")
        
        for epoch in range(epochs):
            epoch_start = time.time()
            
            self.model.train()
            train_loss = 0
            train_batches = 0
            
            for batch_x, batch_y in train_loader:
                batch_x, batch_y = batch_x.to(self.device), batch_y.to(self.device)
                
                optimizer.zero_grad()
                outputs = self.model(batch_x)
                
                loss = self.set_based_loss(outputs, batch_y)
                loss.backward()
                
                torch.nn.utils.clip_grad_norm_(self.model.parameters(), max_norm=1.0)
                optimizer.step()
                
                train_loss += loss.item()
                train_batches += 1
            
            avg_train_loss = train_loss / train_batches if train_batches > 0 else 0
            
            # Validation phase
            self.model.eval()
            val_loss = 0
            val_batches = 0
            val_metrics = defaultdict(float)
            
            with torch.no_grad():
                for batch_x, batch_y in val_loader:
                    batch_x, batch_y = batch_x.to(self.device), batch_y.to(self.device)
                    outputs = self.model(batch_x)
                    
                    loss = self.set_based_loss(outputs, batch_y)
                    val_loss += loss.item()
                    val_batches += 1
                    
                    batch_metrics = metrics_calculator(outputs, batch_y)
                    for key, value in batch_metrics.items():
                        val_metrics[key] += value
            avg_val_loss = val_loss / val_batches if val_batches > 0 else float('inf')
            
            # Average metrics
            for key in val_metrics:
                val_metrics[key] /= val_batches if val_batches > 0 else 1
            
            train_losses.append(avg_train_loss)
            val_losses.append(avg_val_loss)
            val_metrics_history.append(dict(val_metrics))
            
            # Learning rate scheduling
            scheduler.step(avg_val_loss)
            
            # Early stopping
            if avg_val_loss < best_val_loss:
                best_val_loss = avg_val_loss
                patience_counter = 0
                # Save best model
                torch.save({
                    'model_state_dict': self.model.state_dict(),
                    'key_to_idx': self.key_to_idx,
                    'idx_to_key': self.idx_to_key,
                    'window_stats': self.window_stats
                }, 'best_model.pth')
            else:
                patience_counter += 1
            
            # Print progress with metrics
            current_lr = optimizer.param_groups[0]['lr']
            print(f"\nEpoch [{epoch+1}/{epochs}] - {time.time()-epoch_start:.2f}s")
            print(f"  Train Loss: {avg_train_loss:.4f}, Val Loss: {avg_val_loss:.4f}, LR: {current_lr:.6f}")
            print("  Validation Metrics:")
            for k in [self.top_k_hot_keys]:
                print(f"  Top-{k}: ", end="")
                for metric in ['recall', 'precision', 'f1', 'accuracy']:
                    key = f'top_{k}_{metric}'
                    value = val_metrics.get(key, 0)
                    print(f"{metric[:4]}: {value:.4f}  ", end="")
                print()
            
            if patience_counter >= patience:
                print(f'Early stopping at epoch {epoch+1}')
                break
        
        # Load the best model
        if os.path.exists('best_model.pth'):
            checkpoint = torch.load('best_model.pth', map_location=self.device)
            self.model.load_state_dict(checkpoint['model_state_dict'])
            self.key_to_idx = checkpoint['key_to_idx']
            self.idx_to_key = checkpoint['idx_to_key']
            self.window_stats = checkpoint.get('window_stats', {})
            print("Loaded best model weights")
        
        print(f"\nTraining completed in {time.time()-start_time:.2f} seconds")
        return train_losses, val_losses, val_metrics_history
    
    def evaluate_model(self, test_loader):
        """Evaluate the model with various top-k metrics"""
        print("\nEvaluating model...")
        start_time = time.time()
        
        # k_list = [5000,10000]
        k_list = [10000]
        self.model.eval()
        metrics_calculator = KeyAccuracyMetrics(k_list)
        all_metrics = defaultdict(list)
        
        with torch.no_grad():
            for batch_x, batch_y in test_loader:
                batch_x, batch_y = batch_x.to(self.device), batch_y.to(self.device)
                outputs = self.model(batch_x)
                metrics_dict = metrics_calculator(outputs, batch_y)
                for key, value in metrics_dict.items():
                    all_metrics[key].append(value)
        results = {}
        for key, values in all_metrics.items():
            results[key] = np.mean(values)

        print("\nEvaluation Results:")
        print("-" * 60)
        for k in k_list:
            print(f"Top-{k} Metrics:")
            for metric in ['recall', 'precision', 'f1', 'accuracy']:
                key = f'top_{k}_{metric}'
                value = results.get(key, 0)
                print(f"  {metric.capitalize()}: {value:.4f}")
            print()
        
        print(f"Evaluation completed in {time.time()-start_time:.2f} seconds")
        return results
    
    def predict_next_window_hot_keys(self, recent_windows: List[List[str]], top_k: int = None, top_percentage: float = 1) -> Tuple[List[str], np.ndarray]:
        """Predict hot keys for the next window and return top percentage"""
        if top_k is None:
            top_k = self.top_k_hot_keys
        
        if len(recent_windows) < self.sequence_length:
            raise ValueError(f"Need at least {self.sequence_length} recent windows, got {len(recent_windows)}")
        
        input_windows = recent_windows[-self.sequence_length:]
        input_indices = []
        
        for window_keys in input_windows:
            indices = [self.key_to_idx.get(key, 0) for key in window_keys]
            if len(indices) < self.window_size:
                indices = indices + [0] * (self.window_size - len(indices))
            else:
                indices = indices[:self.window_size]
            input_indices.append(indices)
        input_tensor = torch.tensor([input_indices], dtype=torch.long).to(self.device)
        
        self.model.eval()
        with torch.no_grad():
            predictions = self.model(input_tensor)
            predictions = torch.softmax(predictions, dim=-1)
            scores, indices = predictions.topk(top_k, dim=-1)
            
            predicted_keys = []
            prediction_scores = []
            
            for i in range(top_k):
                idx = indices[0, i].item()
                score = scores[0, i].item()
                predicted_keys.append(self.idx_to_key.get(idx, "[UNK]"))
                prediction_scores.append(score)
        
        # prediction_scores = np.array(prediction_scores)
        # num_top_keys = int(len(predicted_keys) * top_percentage)
        # top_predicted_keys = predicted_keys[:num_top_keys]
        # top_prediction_scores = prediction_scores[:num_top_keys]
        # return top_predicted_keys, top_prediction_scores
        return predicted_keys, np.array(prediction_scores)
    
    # def predict_next_window_hot_keys(self, recent_windows: List[List[str]], top_k: int = None) -> Tuple[List[str], np.ndarray]:
    #     """Predict hot keys for the next window"""
    #     if top_k is None:
    #         top_k = self.top_k_hot_keys
        
    #     if len(recent_windows) < self.sequence_length:
    #         raise ValueError(f"Need at least {self.sequence_length} recent windows, got {len(recent_windows)}")
        
    #     input_windows = recent_windows[-self.sequence_length:]
    #     input_indices = []
        
    #     for window_keys in input_windows:
    #         indices = [self.key_to_idx.get(key, 0) for key in window_keys]
    #         if len(indices) < self.window_size:
    #             indices = indices + [0] * (self.window_size - len(indices))
    #         else:
    #             indices = indices[:self.window_size]
    #         input_indices.append(indices)
    #     input_tensor = torch.tensor([input_indices], dtype=torch.long).to(self.device)
        
    #     self.model.eval()
    #     with torch.no_grad():
    #         predictions = self.model(input_tensor)
    #         predictions = torch.softmax(predictions, dim=-1)
    #         scores, indices = predictions.topk(top_k, dim=-1) #这里也可以是测了top-k的一半
            
    #         predicted_key = []
    #         prediction_scores = []
            
    #         for i in range(top_k):
    #             idx = indices[0, i].item()
    #             score = scores[0, i].item()
    #             predicted_key.append(self.idx_to_key.get(idx, "[UNK]"))
    #             prediction_scores.append(score)
        
    #     return predicted_key, np.array(prediction_scores)
    
    def get_recent_windows(self):
        """Get the most recent windows from the log file"""
        try:
            df = pd.read_csv(self.log_file, header=None, names=['key'])
            windows = []
            total_records = len(df)
            window_step = max(1, self.window_size)
            start_idx = max(0, total_records - self.window_size * self.sequence_length)
            for i in range(start_idx, total_records - self.window_size + 1, window_step):
                window_keys = df['key'].iloc[i:i+self.window_size].tolist()
                windows.append(window_keys)
                
            # Return the last sequence_length windows
            return windows[-self.sequence_length:] if len(windows) >= self.sequence_length else windows
            
        except Exception as e:
            print(f"Error getting recent windows: {e}")
            return []
        
                
    def train_and_predict(self, start_idx, end_idx):
        """Process new data from the log file"""
        print(f"Processing new data from index {start_idx} to {end_idx}")
            
        # Read new data from log file
        with open(self.log_file, 'r') as f:
            for _ in range(start_idx):
                line = f.readline()
                if not line:
                    break
            # Read the required lines
            keys = []
            for i in range(end_idx - start_idx):
                line = f.readline().strip()
                if line:
                    keys.append(line)
            
        if not keys:
            print("No new keys to process")
            return
                
        df = pd.DataFrame(keys, columns=['key'])
        sequences = self.preprocess_data(df)
            
        if self.model is None:
            print("Training initial model...")
            train_loader, val_loader, test_loader, = self.create_data_loaders(sequences)
            self.train_model(train_loader, val_loader,epochs=2)
            self.evaluate_model(test_loader)
            self.save_model('initial_model.pth')
        # else:
        #     print("Updating model with new data...")
        #     self.update_model(sequences)
                
        # Predict hot keys
        recent_windows = self.get_recent_windows()
        print(f"Recent windows available for prediction: {len(recent_windows)}")
        
        if recent_windows and len(recent_windows) >= self.sequence_length:
            hot_keys, scores = self.predict_next_window_hot_keys(recent_windows)
            hot_keys_str = ",".join(hot_keys[::]) 
            message = f"HOT_KEYS:{hot_keys_str}END"
            try:
                self.conn.send(message.encode('utf-8'))
            except Exception as e:
                    print(f"Error sending hot keys: {e}")
       
 

    
    
    # def update_model(self, sequences):
    #     """Update the model with new sequences"""
    #     if len(sequences) == 0:
    #         return
            
    #     # Create a small dataset from the new sequences
    #     dataset = KeySequenceDataset(sequences, self.key_to_idx, 
    #                                self.window_size, self.top_k_hot_keys,
    #                                self.prediction_win)
        
    #     # Create a data loader
    #     loader = DataLoader(dataset, batch_size=min(8, len(sequences)), 
    #                       shuffle=True, num_workers=0)
        
    #     # Fine-tune the model
    #     self.fine_tune_model(loader)
    
    # def fine_tune_model(self, loader, epochs=1, lr=0.0001):
    #     """Fine-tune the model with new data"""
    #     if self.model is None:
    #         return
            
    #     optimizer = optim.Adam(self.model.parameters(), lr=lr)
        
    #     self.model.train()
    #     for epoch in range(epochs):
    #         for batch_x, batch_y in loader:
    #             batch_x, batch_y = batch_x.to(self.device), batch_y.to(self.device)
                
    #             optimizer.zero_grad()
    #             outputs = self.model(batch_x)
                
    #             loss = self.set_based_loss(outputs, batch_y)
    #             loss.backward()
                
    #             torch.nn.utils.clip_grad_norm_(self.model.parameters(), max_norm=1.0)
    #             optimizer.step()
                
    #         print(f"Fine-tuning epoch {epoch+1}/{epochs}, Loss: {loss.item():.4f}")
    
    
    def save_model(self, filepath: str):
        """Save the trained model and metadata"""
        checkpoint = {
            'model_state_dict': self.model.state_dict(),
            'key_to_idx': self.key_to_idx,
            'idx_to_key': self.idx_to_key,
            'window_size': self.window_size,
            'sequence_length': self.sequence_length,
            'top_k_hot_keys': self.top_k_hot_keys,
            'vocab_size': len(self.key_to_idx),
            'window_stats': self.window_stats
        }
        torch.save(checkpoint, filepath)
        print(f"Model saved to {filepath}")
    
    def load_model(self, filepath: str):
        """Load a trained model and metadata"""
        checkpoint = torch.load(filepath, map_location=self.device)
        
        self.key_to_idx = checkpoint['key_to_idx']
        self.idx_to_key = checkpoint['idx_to_key']
        self.window_size = checkpoint['window_size']
        self.sequence_length = checkpoint['sequence_length']
        self.top_k_hot_keys = checkpoint['top_k_hot_keys']
        self.window_stats = checkpoint.get('window_stats', {})
        
        vocab_size = checkpoint['vocab_size']
        self.model = EfficientKeyPredictionLSTM(
            vocab_size=vocab_size,
            window_size=self.window_size,
            embedding_dim=16,
            hidden_size=32,
            num_layers=1,
            dropout_rate=0.3
        ).to(self.device)
        self.model.load_state_dict(checkpoint['model_state_dict'])
        
        print(f"Model loaded from {filepath}")
    
    def stop(self):
        """Stop the communication thread"""
        self.running = False
        if self.conn:
            self.conn.close()
        if self.sock:
            self.sock.close()

def main():
    # Create predictor instance
    predictor = HotKeyPredictor(
        # window_size=500,
        # sequence_length=300,
        # top_k_hot_keys=10000,
        # # top_k_hot_keys=4000,
        # prediction_win=300
        window_size=500,
        sequence_length=3000,
        top_k_hot_keys=100000, 
        prediction_win=3000
    )
    
    # # Check if we have a pre-trained model
    # if os.path.exists('best_model.pth'):
    #     print("Loading pre-trained model...")
    #     predictor.load_model('best_model.pth')
    
    # Set up communication with C++
    print("Setting up communication with C++...")
    predictor.setup_communication("127.0.0.1",60001)
    
    # Start listening for messages in a separate thread
    comm_thread = threading.Thread(target=predictor.listen_for_messages)
    comm_thread.daemon = True
    comm_thread.start()
    
    try:
        # Keep the main thread alive
        print("Python predictor is running. Press Ctrl+C to stop.")
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("Stopping predictor...")
        predictor.stop()

if __name__ == "__main__":
    main()

