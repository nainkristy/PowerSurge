"""
Device Type Classifier using TensorFlow
Classifies electrical devices (mixer, hair dryer, phone charger, fridge, etc.)
based on power/current measurements from a CSV file.

Expected CSV columns:
    avg_power, max_current, cycle_duration, avg_phase_angle,
    avg_variation, max_variation, device_type

Usage:
    python device_classifier.py --csv data.csv --mode train
    python device_classifier.py --csv data.csv --mode predict
    python device_classifier.py --csv new_samples.csv --mode predict --model saved_model/
"""

import argparse
import os
import sys
import numpy as np
import pandas as pd
import tensorflow as tf
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import LabelEncoder, StandardScaler
from sklearn.metrics import classification_report, confusion_matrix
import joblib
import json
import matplotlib.pyplot as plt
import seaborn as sns

# ── Column names ──────────────────────────────────────────────────────────────
FEATURE_COLS = [
    "Average Power",       # Average Power (>4 W)   [W]
    "Max Current",         # Max Current             [A]
    "Cycle Duration",      # Cycle Duration          [sec]
    "Average Phase Angle", # Average Phase Angle     [°]
    "Average Variation",   # Average variation       [A]  (Current Var)
    "Max Variation",       # Max variation           [A]  (Current Var)
]
LABEL_COL = "Device"

MODEL_DIR      = "saved_model"
SCALER_PATH    = os.path.join(MODEL_DIR, "scaler.pkl")
ENCODER_PATH   = os.path.join(MODEL_DIR, "label_encoder.pkl")
CLASSES_PATH   = os.path.join(MODEL_DIR, "classes.json")
HISTORY_PLOT   = os.path.join(MODEL_DIR, "training_history.png")
CONFUSION_PLOT = os.path.join(MODEL_DIR, "confusion_matrix.png")


# ── Data loading ──────────────────────────────────────────────────────────────

def load_csv(path: str) -> pd.DataFrame:
    df = pd.read_csv(path)
    missing = [c for c in FEATURE_COLS if c not in df.columns]
    if missing:
        sys.exit(
            f"[ERROR] CSV is missing columns: {missing}\n"
            f"Expected: {FEATURE_COLS + [LABEL_COL]}"
        )
    return df


# ── Model architecture ────────────────────────────────────────────────────────

def build_model(n_features: int, n_classes: int) -> tf.keras.Model:
    model = tf.keras.Sequential([
        tf.keras.layers.Input(shape=(n_features,)),

        tf.keras.layers.Dense(128, activation="relu"),
        tf.keras.layers.BatchNormalization(),
        tf.keras.layers.Dropout(0.3),

        tf.keras.layers.Dense(64, activation="relu"),
        tf.keras.layers.BatchNormalization(),
        tf.keras.layers.Dropout(0.2),

        tf.keras.layers.Dense(32, activation="relu"),
        tf.keras.layers.Dropout(0.1),

        tf.keras.layers.Dense(n_classes, activation="softmax"),
    ])

    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    return model


# ── Plotting helpers ──────────────────────────────────────────────────────────

def plot_history(history, save_path: str):
    fig, axes = plt.subplots(1, 2, figsize=(12, 4))

    axes[0].plot(history.history["accuracy"],     label="Train")
    axes[0].plot(history.history["val_accuracy"], label="Val")
    axes[0].set_title("Accuracy")
    axes[0].set_xlabel("Epoch")
    axes[0].legend()

    axes[1].plot(history.history["loss"],     label="Train")
    axes[1].plot(history.history["val_loss"], label="Val")
    axes[1].set_title("Loss")
    axes[1].set_xlabel("Epoch")
    axes[1].legend()

    plt.tight_layout()
    plt.savefig(save_path)
    plt.close()
    print(f"[INFO] Training history saved → {save_path}")


def plot_confusion(y_true, y_pred, class_names, save_path: str):
    cm = confusion_matrix(y_true, y_pred)
    fig, ax = plt.subplots(figsize=(max(6, len(class_names)), max(5, len(class_names) - 1)))
    sns.heatmap(
        cm, annot=True, fmt="d", cmap="Blues",
        xticklabels=class_names, yticklabels=class_names, ax=ax
    )
    ax.set_xlabel("Predicted")
    ax.set_ylabel("Actual")
    ax.set_title("Confusion Matrix")
    plt.tight_layout()
    plt.savefig(save_path)
    plt.close()
    print(f"[INFO] Confusion matrix saved → {save_path}")


# ── Training ──────────────────────────────────────────────────────────────────

def train(csv_path: str, epochs: int = 100, batch_size: int = 32):
    print(f"\n[INFO] Loading data from: {csv_path}")
    df = load_csv(csv_path)

    if LABEL_COL not in df.columns:
        sys.exit(f"[ERROR] Label column '{LABEL_COL}' not found in CSV.")

    df = df.dropna(subset=FEATURE_COLS + [LABEL_COL])
    print(f"[INFO] Samples: {len(df)} | Classes: {df[LABEL_COL].nunique()}")
    print(f"[INFO] Class distribution:\n{df[LABEL_COL].value_counts().to_string()}\n")

    X = df[FEATURE_COLS].values.astype(np.float32)
    y_raw = df[LABEL_COL].values

    # Encode labels
    le = LabelEncoder()
    y = le.fit_transform(y_raw)
    class_names = list(le.classes_)
    print(f"[INFO] Classes: {class_names}")

    # Scale features
    scaler = StandardScaler()
    X_scaled = scaler.fit_transform(X)

    # Train / validation split
    X_train, X_val, y_train, y_val = train_test_split(
        X_scaled, y, test_size=0.2, random_state=42, stratify=y
    )

    # Build model
    model = build_model(n_features=len(FEATURE_COLS), n_classes=len(class_names))
    model.summary()

    callbacks = [
        tf.keras.callbacks.EarlyStopping(
            monitor="val_loss", patience=15, restore_best_weights=True
        ),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor="val_loss", factor=0.5, patience=7, min_lr=1e-6
        ),
    ]

    print("\n[INFO] Training …")
    history = model.fit(
        X_train, y_train,
        validation_data=(X_val, y_val),
        epochs=epochs,
        batch_size=batch_size,
        callbacks=callbacks,
        verbose=1,
    )

    # Evaluate
    print("\n[INFO] Evaluation on validation set:")
    loss, acc = model.evaluate(X_val, y_val, verbose=0)
    print(f"  Accuracy : {acc:.4f}")
    print(f"  Loss     : {loss:.4f}\n")

    y_pred = np.argmax(model.predict(X_val, verbose=0), axis=1)
    print(classification_report(y_val, y_pred, target_names=class_names))

    # Save artefacts
    os.makedirs(MODEL_DIR, exist_ok=True)
    model.save(os.path.join(MODEL_DIR, "model.keras"))
    joblib.dump(scaler, SCALER_PATH)
    joblib.dump(le, ENCODER_PATH)
    with open(CLASSES_PATH, "w") as f:
        json.dump(class_names, f, indent=2)

    print(f"[INFO] Model saved → {MODEL_DIR}/")
    plot_history(history, HISTORY_PLOT)
    plot_confusion(y_val, y_pred, class_names, CONFUSION_PLOT)


# ── Prediction ────────────────────────────────────────────────────────────────

def predict(csv_path: str):
    model_path = os.path.join(MODEL_DIR, "model.keras")
    for p in (model_path, SCALER_PATH, ENCODER_PATH, CLASSES_PATH):
        if not os.path.exists(p):
            sys.exit(
                f"[ERROR] Missing file: {p}\n"
                f"Run with --mode train first."
            )

    print(f"\n[INFO] Loading model from: {MODEL_DIR}/")
    model   = tf.keras.models.load_model(model_path)
    scaler  = joblib.load(SCALER_PATH)
    le      = joblib.load(ENCODER_PATH)
    with open(CLASSES_PATH) as f:
        class_names = json.load(f)

    print(f"[INFO] Loading data from: {csv_path}")
    df = load_csv(csv_path)
    df = df.dropna(subset=FEATURE_COLS)

    X = df[FEATURE_COLS].values.astype(np.float32)
    X_scaled = scaler.transform(X)

    probs      = model.predict(X_scaled, verbose=0)
    pred_idx   = np.argmax(probs, axis=1)
    pred_label = le.inverse_transform(pred_idx)
    confidence = probs[np.arange(len(probs)), pred_idx]

    df["predicted_device"] = pred_label
    df["confidence"]       = confidence.round(4)

    # Console output
    print(f"\n{'#'*60}")
    print(f"  Predictions ({len(df)} samples)")
    print(f"{'#'*60}")
    for _, row in df.iterrows():
        print(
            f"  {row['predicted_device']:<20} "
            f"(confidence: {row['confidence']:.2%})  |  "
            + "  ".join(f"{c}={row[c]:.4f}" for c in FEATURE_COLS)
        )

    out_csv = "predictions.csv"
    df.to_csv(out_csv, index=False)
    print(f"\n[INFO] Predictions saved → {out_csv}")


# ── Synthetic demo data ───────────────────────────────────────────────────────

def generate_demo_csv(path: str = "demo_data.csv"):
    """
    Creates a small synthetic dataset so the script can be tested immediately.
    Replace with real measured data for production use.
    """
    rng = np.random.default_rng(0)

    # (device, avg_power, max_current, cycle_dur, phase_angle, avg_var, max_var)
    profiles = {
        "mixer":         (350,  2.0,  0.5,  15,  0.05, 0.12),
        "hair_dryer":    (1500, 7.0,  0.2,   5,  0.02, 0.05),
        "phone_charger": (10,   0.05, 0.01,  2,  0.001,0.003),
        "fridge":        (150,  0.8,  6.0,  30,  0.01, 0.03),
        "washing_machine":(800, 4.5,  60.0, 20,  0.08, 0.20),
        "laptop":        (60,   0.3,  0.05,  3,  0.005,0.010),
        "led_bulb":      (10,   0.04, 0.02,  1,  0.0005,0.001),
        "microwave":     (1000, 4.8,  0.3,   2,  0.03, 0.08),
    }

    rows = []
    n_per_class = 80
    for device, (pw, mc, cd, pa, av, mv) in profiles.items():
        for _ in range(n_per_class):
            rows.append({
                "Average Power":       max(0, rng.normal(pw, pw * 0.05)),
                "Max Current":         max(0, rng.normal(mc, mc * 0.05)),
                "Cycle Duration":      max(0, rng.normal(cd, cd * 0.05)),
                "Average Phase Angle": rng.normal(pa, pa * 0.05),
                "Average Variation":   max(0, rng.normal(av, av * 0.1)),
                "Max Variation":       max(0, rng.normal(mv, mv * 0.1)),
                "Device":              device,
            })

    pd.DataFrame(rows).sample(frac=1, random_state=0).to_csv(path, index=False)
    print(f"[INFO] Demo CSV created → {path}  ({len(rows)} samples, {len(profiles)} classes)")


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Device Type Classifier")
    parser.add_argument("--csv",        default="demo_data.csv", help="Path to CSV file")
    parser.add_argument("--mode",       choices=["train", "predict", "demo"], default="demo",
                        help="train: fit model | predict: run inference | demo: generate sample data + train")
    parser.add_argument("--epochs",     type=int, default=100)
    parser.add_argument("--batch_size", type=int, default=32)
    args = parser.parse_args()

    if args.mode == "demo":
        generate_demo_csv(args.csv)
        train(args.csv, args.epochs, args.batch_size)

    elif args.mode == "train":
        train(args.csv, args.epochs, args.batch_size)

    elif args.mode == "predict":
        predict(args.csv)


if __name__ == "__main__":
    main()