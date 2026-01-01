#!/usr/bin/env python3
"""
irrigation_firestore_ml_final.py

Same as your previous version but now also listens to:
 - /farms/{FARM_ID} (the farm doc) for changes to metadata fields (crop, initial_height, plant_date, size, ph, etc.)
 - /farms/{FARM_ID}/readings/{today_key} (the day's reading doc) for any top-level changes (avg_humidity, sensors map, etc.)

When any of these docs change the script will recompute predictions and update /predictions/latest.

No other logic changed.
"""
import os
import time
import joblib
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo
from dateutil import parser as dateparser

import numpy as np
import pandas as pd

from sklearn.pipeline import Pipeline
from sklearn.compose import ColumnTransformer
from sklearn.preprocessing import OneHotEncoder, StandardScaler
from sklearn.ensemble import RandomForestClassifier, RandomForestRegressor
from sklearn.model_selection import train_test_split, cross_val_score
from sklearn.metrics import classification_report, accuracy_score, mean_absolute_error, r2_score

import firebase_admin
from firebase_admin import credentials, firestore

# ---------------- USER CONFIG - EDIT BEFORE RUNNING ----------------
SERVICE_ACCOUNT = "./serviceAccount.json"   # path to your service account JSON
FARM_ID = "unalytix@gmail.com"              # your farm document id
# Put your dataset path here. Can be .csv or .xlsx/.xls
DATASET_PATH = "./irrigation_data.xlsx"
MODEL_DIR = "./models"                      # where models will be loaded/saved
STAGE_MODEL_FILE = os.path.join(MODEL_DIR, "stage_clf_dataset_only.joblib")
WATER_MODEL_FILE = os.path.join(MODEL_DIR, "water_reg_dataset_only.joblib")
os.makedirs(MODEL_DIR, exist_ok=True)
# ------------------------------------------------------------------

# Constants and behavior tuning
HARARE = ZoneInfo("Africa/Harare")
FARM_M2_PER_ACRE = 4046.8564224
RAIN_EFFECTIVENESS = 0.6      # fraction of rain that effectively reduces irrigation need
RAIN_LOW_THRESHOLD = 0.20     # if rain_prob < this, do not adjust expected_mm_per_m2 (very low)
MIN_EVAP_DAYS_EPS = 1.0 / 1440.0  # min elapsed days to avoid div-by-zero (1 minute)
STAGE_THRESHOLDS = {"germination": 25.0, "vegetative": 30.0, "flowering": 35.0, "maturity": 28.0}
# ------------------------------------------------------------------

# Firestore init
if not os.path.exists(SERVICE_ACCOUNT):
    raise FileNotFoundError(f"Service account JSON not found at {SERVICE_ACCOUNT}")
cred = credentials.Certificate(SERVICE_ACCOUNT)
if not firebase_admin._apps:
    firebase_admin.initialize_app(cred)
fs = firestore.client()

PRED_LATEST_REF = fs.collection("farms").document(FARM_ID).collection("predictions").document("latest")
FARM_DOC_REF = fs.collection("farms").document(FARM_ID)

# Runtime persisted state (will persist to Firestore state doc)
STATE_DOC_REF = fs.collection("farms").document(FARM_ID).collection("predictions").document("state")

LAST_INITIAL_MOISTURE = None   # percentage points (e.g., 78.3)
LAST_INITIAL_TIME = None       # tz-aware datetime in HARARE
EVAP_PP_PER_DAY = 0.0          # percentage points/day (starts at 0)

# ---------------------- Utility helpers -----------------------------
def today_date_key_harare():
    """Return date key like '09nov2025' for today's date in Harare timezone."""
    return datetime.now(HARARE).strftime("%d%b%Y").lower()

def sensor_doc_ref(farm_id, date_key, sensor_name):
    return fs.collection("farms").document(farm_id).collection("readings").document(date_key).collection("sensors").document(sensor_name)

def reading_doc_ref(farm_id, date_key):
    return fs.collection("farms").document(farm_id).collection("readings").document(date_key)

def safe_float(v):
    try:
        return float(v)
    except Exception:
        return None

def persist_state():
    global LAST_INITIAL_MOISTURE, LAST_INITIAL_TIME, EVAP_PP_PER_DAY
    try:
        payload = {
            "last_initial_moisture": float(LAST_INITIAL_MOISTURE) if LAST_INITIAL_MOISTURE is not None else None,
            "last_initial_time": LAST_INITIAL_TIME.isoformat() if LAST_INITIAL_TIME is not None else None,
            "evap_pp_per_day": float(EVAP_PP_PER_DAY)
        }
        STATE_DOC_REF.set(payload)
        print("[state] persisted")
    except Exception as e:
        print("[state] persist error:", e)

def load_state():
    global LAST_INITIAL_MOISTURE, LAST_INITIAL_TIME, EVAP_PP_PER_DAY
    try:
        doc = STATE_DOC_REF.get()
        if doc.exists:
            d = doc.to_dict() or {}
            LAST_INITIAL_MOISTURE = safe_float(d.get("last_initial_moisture"))
            t = d.get("last_initial_time")
            if t:
                try:
                    parsed = dateparser.isoparse(t)
                    if parsed.tzinfo is None:
                        parsed = parsed.replace(tzinfo=HARARE)
                    LAST_INITIAL_TIME = parsed.astimezone(HARARE)
                except Exception:
                    LAST_INITIAL_TIME = datetime.now(HARARE)
            EVAP_PP_PER_DAY = float(d.get("evap_pp_per_day") or 0.0)
            print("[state] loaded:", {"last_initial_moisture": LAST_INITIAL_MOISTURE, "evap_pp_per_day": EVAP_PP_PER_DAY})
            return
    except Exception as e:
        print("[state] load failed:", e)
    # seed from today's soil doc if available
    seed_from_soil()

def seed_from_soil():
    global LAST_INITIAL_MOISTURE, LAST_INITIAL_TIME, EVAP_PP_PER_DAY
    LAST_INITIAL_MOISTURE = None
    LAST_INITIAL_TIME = None
    EVAP_PP_PER_DAY = 0.0
    date_key = today_date_key_harare()
    try:
        sdoc = sensor_doc_ref(FARM_ID, date_key, "soil").get()
        if sdoc.exists:
            d = sdoc.to_dict() or {}
            val = d.get("current")
            LAST_INITIAL_MOISTURE = safe_float(val)
            ts = d.get("timestamp") or d.get("time")
            if ts:
                try:
                    t = dateparser.isoparse(str(ts))
                    if t.tzinfo is None:
                        t = t.replace(tzinfo=HARARE)
                    LAST_INITIAL_TIME = t.astimezone(HARARE)
                except Exception:
                    LAST_INITIAL_TIME = datetime.now(HARARE)
    except Exception:
        pass
    if LAST_INITIAL_TIME is None:
        LAST_INITIAL_TIME = datetime.now(HARARE)
    print("[state] seeded baseline:", {"last_initial_moisture": LAST_INITIAL_MOISTURE, "last_initial_time": LAST_INITIAL_TIME})

# ------------------ ML: read dataset robustly & train (dataset-only) -----------------------
def make_onehot_encoder():
    try:
        return OneHotEncoder(handle_unknown="ignore", sparse_output=False)
    except TypeError:
        return OneHotEncoder(handle_unknown="ignore", sparse=False)

def _read_dataset(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"Dataset not found at {path}")
    ext = os.path.splitext(path)[1].lower()
    if ext in (".xls", ".xlsx"):
        try:
            return pd.read_excel(path, engine="openpyxl")
        except ImportError as e:
            raise ImportError("Reading Excel files requires 'openpyxl'. Install with `pip install openpyxl` and retry.") from e
        except Exception as e:
            try:
                return pd.read_excel(path)
            except Exception as e2:
                raise RuntimeError(f"Failed to read Excel file {path}: {e2}") from e2
    else:
        encodings = ["utf-8-sig", "utf-8", "latin1", "cp1252"]
        last_err = None
        for enc in encodings:
            try:
                df = pd.read_csv(path, encoding=enc, engine="python")
                return df
            except UnicodeDecodeError as ude:
                last_err = ude
                continue
            except Exception as e:
                last_err = e
                continue
        raise RuntimeError(f"Failed to read CSV file {path} with tried encodings {encodings}. Last error: {last_err}")

def load_or_train_models():
    stage_model = None
    water_model = None
    if os.path.exists(STAGE_MODEL_FILE):
        try:
            stage_model = joblib.load(STAGE_MODEL_FILE)
            print(f"[ml] loaded stage model from {STAGE_MODEL_FILE}")
        except Exception as e:
            print("[ml] failed to load stage model:", e)
            stage_model = None
    if os.path.exists(WATER_MODEL_FILE):
        try:
            water_model = joblib.load(WATER_MODEL_FILE)
            print(f"[ml] loaded water model from {WATER_MODEL_FILE}")
        except Exception as e:
            print("[ml] failed to load water model:", e)
            water_model = None

    if (stage_model is None or water_model is None):
        if not os.path.exists(DATASET_PATH):
            missing = []
            if stage_model is None: missing.append("stage model")
            if water_model is None: missing.append("water model")
            raise FileNotFoundError(f"Missing {', '.join(missing)} and dataset not found at {DATASET_PATH}. I will not synthesize data.")
        print("[ml] (re)training models from dataset (dataset-only)...")
        df = _read_dataset(DATASET_PATH)
        df.columns = [c.strip() for c in df.columns]

        def find(cands):
            for c in cands:
                if c in df.columns: return c
            return None
        COL_CROP = find(["Crop Name","crop","crop_name"])
        COL_SOIL_TYPE = find(["Soil Type","soil_type","soil"])
        COL_SOIL_PH = find(["Soil pH","soil_pH","ph"])
        COL_INIT_H = find(["Initial Height (cm)","Initial Height","initial_height","initial_height_cm","Initial Height (cm) "])
        COL_VEG_R = find(["Vegetative Height Range (cm)","Vegetative Height Range","Vegetative Height Range (cm)"])
        COL_FLOW_R = find(["Flowering Height Range (cm)","Flowering Height Range"])
        COL_MAT_R = find(["Maturity Height Range (cm)","Maturity Height Range"])
        COL_GERM_MM = find(["Germination Daily Water (mm/m2)","Germination Daily Water (mm)"])
        COL_VEG_MM = find(["Vegetative Daily Water (mm/m2)","Vegetative Daily Water (mm)"])
        COL_FLOW_MM = find(["Flowering Daily Water (mm/m2)","Flowering Daily Water (mm)"])
        COL_MAT_MM = find(["Maturity Daily Water (mm/m2)","Maturity Daily Water (mm)"])

        def parse_range_field(val):
            if pd.isna(val) or val is None: return None
            s = str(val).strip().replace(" ", "").strip("'\"")
            if "-" in s:
                try:
                    a,b = s.split("-",1); return (int(float(a)), int(float(b)))
                except:
                    return None
            try:
                n = int(float(s)); return (n,n)
            except:
                return None

        ex_rows = []
        if COL_CROP and COL_INIT_H and COL_VEG_R:
            grouped = df.groupby(COL_CROP)
            for _, group in grouped:
                for _, r in group.iterrows():
                    crop = str(r.get(COL_CROP,"")).strip().lower()
                    soil = str(r.get(COL_SOIL_TYPE,"")).strip().lower() if COL_SOIL_TYPE else "loam"
                    ph = safe_float(r.get(COL_SOIL_PH)) if COL_SOIL_PH else 6.5
                    init_h = safe_float(r.get(COL_INIT_H))
                    veg_r = parse_range_field(r.get(COL_VEG_R))
                    flow_r = parse_range_field(r.get(COL_FLOW_R)) if COL_FLOW_R else None
                    mat_r = parse_range_field(r.get(COL_MAT_R)) if COL_MAT_R else None
                    if init_h is None or veg_r is None:
                        continue
                    if init_h == 0:
                        a = 0; b = veg_r[0]-1 if veg_r[0] > 0 else 0
                        if b >= a:
                            pts = sorted(list({a, (a+b)//2, b}))
                            for ch in pts:
                                ex_rows.append({"crop":crop,"soil_type":soil,"soil_pH":ph,"days_since":5,"initial_height_cm":0,"current_height_cm":int(ch),"stage":"germination"})
                    a,b = veg_r
                    pts = sorted(list({int(a), int((a+b)//2), int(b)}))
                    for ch in pts:
                        ex_rows.append({"crop":crop,"soil_type":soil,"soil_pH":ph,"days_since":25,"initial_height_cm":int(init_h),"current_height_cm":int(ch),"stage":"vegetative"})
                    if flow_r is not None:
                        a,b = flow_r
                        pts = sorted(list({int(a), int((a+b)//2), int(b)}))
                        for ch in pts:
                            ex_rows.append({"crop":crop,"soil_type":soil,"soil_pH":ph,"days_since":60,"initial_height_cm":int(init_h),"current_height_cm":int(ch),"stage":"flowering"})
                    if mat_r is not None:
                        a,b = mat_r
                        pts = sorted(list({int(a), int((a+b)//2), int(b)}))
                        for ch in pts:
                            ex_rows.append({"crop":crop,"soil_type":soil,"soil_pH":ph,"days_since":100,"initial_height_cm":int(init_h),"current_height_cm":int(ch),"stage":"maturity"})
        ex_df = pd.DataFrame(ex_rows)

        wrows = []
        if COL_CROP:
            for _, r in df.iterrows():
                crop = str(r.get(COL_CROP,"")).strip().lower()
                soil = str(r.get(COL_SOIL_TYPE,"")).strip().lower() if COL_SOIL_TYPE else "loam"
                ph = safe_float(r.get(COL_SOIL_PH)) if COL_SOIL_PH else 6.5
                mapping = {"germination": COL_GERM_MM, "vegetative": COL_VEG_MM, "flowering": COL_FLOW_MM, "maturity": COL_MAT_MM}
                for st, col in mapping.items():
                    if col and col in r and not pd.isna(r[col]):
                        v = safe_float(r[col])
                        wrows.append({"crop":crop,"stage":st,"soil_type":soil,"soil_pH":ph,"mm":float(v)})
        wtr = pd.DataFrame(wrows)

        if stage_model is None:
            if ex_df.empty:
                raise RuntimeError("Stage model missing and dataset -> could not build stage examples from dataset. Ensure Initial Height and Vegetative Height Range columns present.")
            Xs = ex_df[["crop","soil_type","soil_pH","days_since","initial_height_cm","current_height_cm"]]
            ys = ex_df["stage"]
            preproc = ColumnTransformer([("cat", make_onehot_encoder(), ["crop","soil_type"]), ("num", StandardScaler(), ["soil_pH","days_since","initial_height_cm","current_height_cm"])])
            clf = Pipeline([("pre", preproc), ("clf", RandomForestClassifier(n_estimators=150, random_state=42))])
            n_samples = len(ex_df); n_classes = len(ys.unique())
            if n_samples * 0.2 < n_classes or n_samples < n_classes*3:
                k = min(5, max(2, n_samples))
                try:
                    scores = cross_val_score(clf, Xs, ys, cv=k, scoring="accuracy")
                    print("[ml] stage CV accuracies:", scores, "mean:", scores.mean())
                except Exception as e:
                    print("[ml] stage CV failed:", e)
                clf.fit(Xs, ys)
                preds = clf.predict(Xs)
                acc = accuracy_score(ys, preds)
                print(f"[ml] stage accuracy on training examples: {acc:.3f}")
                print(classification_report(ys, preds, zero_division=0))
            else:
                Xtr, Xte, ytr, yte = train_test_split(Xs, ys, test_size=0.2, random_state=42, stratify=ys)
                clf.fit(Xtr, ytr)
                preds = clf.predict(Xte)
                acc = accuracy_score(yte, preds)
                print(f"[ml] stage test accuracy: {acc:.3f}")
                print(classification_report(yte, preds, zero_division=0))
            stage_model = clf
            joblib.dump(stage_model, STAGE_MODEL_FILE)
            print(f"[ml] stage model trained & saved -> {STAGE_MODEL_FILE}")

        if water_model is None:
            if wtr.empty:
                raise RuntimeError("Water model missing and dataset -> could not build water training rows from dataset. Ensure per-stage mm columns present.")
            Xw = wtr[["crop","stage","soil_type","soil_pH"]]
            yw = wtr["mm"]
            preproc_w = ColumnTransformer([("cat", make_onehot_encoder(), ["crop","stage","soil_type"]), ("num", StandardScaler(), ["soil_pH"])])
            reg = Pipeline([("pre", preproc_w), ("reg", RandomForestRegressor(n_estimators=150, random_state=42))])
            if len(wtr) > 1:
                Xtr, Xte, ytr, yte = train_test_split(Xw, yw, test_size=0.2, random_state=42)
                reg.fit(Xtr, ytr)
                ypred = reg.predict(Xte)
                mae = mean_absolute_error(yte, ypred)
                r2 = r2_score(yte, ypred) if len(yte) > 1 else float("nan")
                print(f"[ml] water regressor MAE: {mae:.3f}, R2: {r2:.3f}")
            else:
                reg.fit(Xw, yw)
                print("[ml] water trained on tiny dataset")
            water_model = reg
            joblib.dump(water_model, WATER_MODEL_FILE)
            print(f"[ml] water model trained & saved -> {WATER_MODEL_FILE}")

    if stage_model is None or water_model is None:
        raise RuntimeError("Failed to obtain both stage and water models. Check dataset and paths.")
    return stage_model, water_model

# ------------------- Rain & evap helpers -----------------------------------
def compute_rain_probability_from_humidity(current_humidity):
    if current_humidity is None:
        return 0.05
    h = float(current_humidity)
    if h >= 85: return 0.90
    if h >= 75: return 0.65
    if h >= 65: return 0.35
    if h >= 55: return 0.15
    return 0.03

def theoretical_evap_pp_day_from_temp(temp_c):
    if temp_c is None:
        temp_c = 25.0
    base_pp_hour = (0.40 * 100.0) / 2.0
    return float(base_pp_hour * (temp_c / 25.0) * 24.0)

# ------------------- Main recompute & publish logic ------------------------
def recompute_and_publish(trigger_source="manual"):
    global LAST_INITIAL_MOISTURE, LAST_INITIAL_TIME, EVAP_PP_PER_DAY

    now = datetime.now(HARARE)
    date_key = today_date_key_harare()

    farm_doc = FARM_DOC_REF.get()
    farm_meta = farm_doc.to_dict() if farm_doc.exists else {}
    crop = (farm_meta.get("crop") or farm_meta.get("Crop") or "").strip().lower() or "maize"
    soil_type = (farm_meta.get("soilType") or farm_meta.get("soil_type") or "loam").strip().lower()
    ph = safe_float(farm_meta.get("ph") or farm_meta.get("soil_pH") or 6.5)

    farm_size_acres = safe_float(farm_meta.get("size") or farm_meta.get("Size") or farm_meta.get("farm_size_acres") or 1.0) or 1.0
    if ("size" not in farm_meta) and ("Size" not in farm_meta):
        print("[warn] farm doc does not contain 'size' field; defaulting to 1.0 acres")

    initial_height = safe_float(farm_meta.get("initial_height") or farm_meta.get("Initial Height (cm)") or farm_meta.get("initialHeight") or 0.0)
    current_height = safe_float(farm_meta.get("current_height") or farm_meta.get("currentHeight") or farm_meta.get("current_height_cm") or initial_height or 0.0)

    plant_date_raw = farm_meta.get("plant_date") or farm_meta.get("plantDate") or farm_meta.get("plantation_date")
    plant_date = None
    if plant_date_raw:
        try:
            parsed = dateparser.parse(str(plant_date_raw), fuzzy=True)
            if parsed.tzinfo is not None:
                parsed_harare = parsed.astimezone(HARARE)
            else:
                parsed_harare = parsed.replace(tzinfo=HARARE)
            plant_date = parsed_harare.date()
            print(f"[info] parsed plant_date '{plant_date_raw}' -> {plant_date} (Harare)")
        except Exception as e:
            print(f"[warn] failed to parse plant_date '{plant_date_raw}': {e}")
            plant_date = None
    else:
        print("[info] plant_date not present in farm doc")

    days_since = (datetime.now(HARARE).date() - plant_date).days if plant_date else 0

    temp = None; humidity = None; current_moisture = None
    try:
        tdoc = sensor_doc_ref(FARM_ID, date_key, "temperature").get()
        if tdoc.exists:
            temp = safe_float((tdoc.to_dict() or {}).get("current"))
    except Exception:
        pass
    try:
        hdoc = sensor_doc_ref(FARM_ID, date_key, "humidity").get()
        if hdoc.exists:
            humidity = safe_float((hdoc.to_dict() or {}).get("current"))
    except Exception:
        pass
    try:
        sdoc = sensor_doc_ref(FARM_ID, date_key, "soil").get()
        if sdoc.exists:
            current_moisture = safe_float((sdoc.to_dict() or {}).get("current"))
    except Exception:
        pass

    try:
        r_doc = reading_doc_ref(FARM_ID, date_key).get()
        if r_doc.exists:
            rd = r_doc.to_dict() or {}
            if temp is None and "temperature" in rd: temp = safe_float(rd.get("temperature"))
            if humidity is None and "humidity" in rd: humidity = safe_float(rd.get("humidity"))
            if current_moisture is None:
                s = rd.get("sensors")
                if isinstance(s, dict):
                    node = s.get("soil") or s.get("moisture") or s.get("soil_moisture")
                    if isinstance(node, dict):
                        current_moisture = safe_float(node.get("current"))
                    else:
                        current_moisture = safe_float(node)
    except Exception:
        pass

    stage_model, water_model = load_or_train_models()
    X_stage = pd.DataFrame([{
        "crop": crop,
        "soil_type": soil_type,
        "soil_pH": ph if ph is not None else 6.5,
        "days_since": float(days_since),
        "initial_height_cm": float(initial_height if initial_height is not None else 0.0),
        "current_height_cm": float(current_height if current_height is not None else 0.0)
    }])
    try:
        proba = stage_model.predict_proba(X_stage)[0]
        classes = stage_model.named_steps["clf"].classes_
        idx = int(np.argmax(proba))
        predicted_stage = str(classes[idx])
        stage_conf = float(proba[idx])
    except Exception:
        try:
            predicted_stage = str(stage_model.predict(X_stage)[0])
            stage_conf = 0.0
        except Exception:
            predicted_stage = "vegetative"
            stage_conf = 0.0

    X_reg = pd.DataFrame([{"crop": crop, "stage": predicted_stage, "soil_type": soil_type, "soil_pH": ph if ph is not None else 6.5}])
    try:
        required_mm_per_m2 = float(water_model.predict(X_reg)[0])
    except Exception:
        fallback = {
            "maize": {"germination":2.5,"vegetative":4.5,"flowering":6.0,"maturity":3.0},
            "sorghum": {"germination":2.5,"vegetative":4.0,"flowering":5.5,"maturity":3.0},
            "wheat": {"germination":2.0,"vegetative":3.5,"flowering":5.5,"maturity":2.5},
            "beans": {"germination":1.5,"vegetative":3.0,"flowering":4.0,"maturity":2.5},
            "tomato": {"germination":2.0,"vegetative":3.5,"flowering":5.0,"maturity":3.5}
        }
        required_mm_per_m2 = float(fallback.get(crop, fallback["maize"]).get(predicted_stage, 4.5))

    evap_updated = False
    if LAST_INITIAL_MOISTURE is not None and current_moisture is not None and current_moisture + 1e-9 < LAST_INITIAL_MOISTURE:
        elapsed_days = (datetime.now(HARARE) - LAST_INITIAL_TIME).total_seconds() / 86400.0
        elapsed_days = max(elapsed_days, MIN_EVAP_DAYS_EPS)
        observed_pp_day = max(0.0, (LAST_INITIAL_MOISTURE - current_moisture) / elapsed_days)
        EVAP_PP_PER_DAY = float(observed_pp_day)
        LAST_INITIAL_MOISTURE = current_moisture
        LAST_INITIAL_TIME = datetime.now(HARARE)
        evap_updated = True
        persist_state()
    elif LAST_INITIAL_MOISTURE is None and current_moisture is not None:
        LAST_INITIAL_MOISTURE = current_moisture
        LAST_INITIAL_TIME = datetime.now(HARARE)

    rain_prob = compute_rain_probability_from_humidity(humidity)

    if rain_prob < RAIN_LOW_THRESHOLD:
        expected_mm_per_m2 = required_mm_per_m2
    else:
        expected_mm_per_m2 = float(required_mm_per_m2 * max(0.0, (1.0 - rain_prob * RAIN_EFFECTIVENESS)))

    threshold = STAGE_THRESHOLDS.get(predicted_stage, 30.0)
    predicted_time_iso = None
    predicted_time_to_threshold_hours = None
    if current_moisture is None:
        predicted_time_iso = None
        predicted_time_to_threshold_hours = None
    else:
        if EVAP_PP_PER_DAY > 0.0:
            if current_moisture <= threshold:
                predicted_time_to_threshold_hours = 0.0
                predicted_time_iso = datetime.now(HARARE).isoformat()
            else:
                days = (current_moisture - threshold) / EVAP_PP_PER_DAY
                hours = max(0.0, days * 24.0)
                predicted_time_to_threshold_hours = float(round(hours,3))
                predicted_time_iso = (datetime.now(HARARE) + timedelta(hours=hours)).astimezone(HARARE).isoformat()
        else:
            theor_pp_day = theoretical_evap_pp_day_from_temp(temp)
            if theor_pp_day <= 0:
                predicted_time_to_threshold_hours = 72.0
                predicted_time_iso = (datetime.now(HARARE) + timedelta(hours=72)).astimezone(HARARE).isoformat()
            else:
                if current_moisture <= threshold:
                    predicted_time_to_threshold_hours = 0.0
                    predicted_time_iso = datetime.now(HARARE).isoformat()
                else:
                    days = (current_moisture - threshold) / theor_pp_day
                    hours = max(0.0, days * 24.0)
                    predicted_time_to_threshold_hours = float(round(hours, 3))
                    predicted_time_iso = (datetime.now(HARARE) + timedelta(hours=hours)).astimezone(HARARE).isoformat()

    obs_lines = []
    obs_lines.append(f"I predicted '{predicted_stage}' stage using initial height and current height.")
    obs_lines.append(f"Current soil moisture is {current_moisture if current_moisture is not None else 'unknown'}% and baseline moisture tracking starts at {LAST_INITIAL_MOISTURE if LAST_INITIAL_MOISTURE is not None else 'unknown'}%.")
    if EVAP_PP_PER_DAY > 0:
        obs_lines.append(f"Soil is drying at about {EVAP_PP_PER_DAY:.2f} percentage-points per day, which determines irrigation timing.")
    else:
        if temp is not None:
            obs_lines.append(f"No drying observed yet; using temperature ({temp:.1f}°C) to estimate when moisture will drop.")
        else:
            obs_lines.append("No drying observed and temperature unknown, using a conservative timing estimate.")
    if rain_prob >= RAIN_LOW_THRESHOLD:
        obs_lines.append(f"Rain chance is moderate/high ({int(rain_prob*100)}%), so irrigation amount is reduced accordingly.")
    else:
        obs_lines.append(f"Rain chance is low ({int(rain_prob*100)}%), so irrigation amount uses required baseline.")
    obs_lines.append(f"Required (baseline) water: {required_mm_per_m2:.2f} mm/m²; planned (expected) water: {expected_mm_per_m2:.2f} mm/m².")
    if predicted_time_iso:
        obs_lines.append(f"Predicted irrigation around {predicted_time_iso} Harare time.")
    observations = " ".join(obs_lines)

    try:
        FARM_DOC_REF.set({"predicted_stage": predicted_stage, "stage_confidence": float(stage_conf)}, merge=True)
    except Exception as e:
        print("[warn] failed to update farm doc with predicted_stage:", e)

    payload = {
        "trigger_source": trigger_source,
        "last_updated_harare": datetime.now(HARARE).isoformat(),
        "rain_probability": float(round(rain_prob, 3)),
        "initial_moisture_tracked": float(LAST_INITIAL_MOISTURE) if LAST_INITIAL_MOISTURE is not None else None,
        "current_moisture": float(current_moisture) if current_moisture is not None else None,
        "temperature": float(temp) if temp is not None else None,
        "humidity": float(humidity) if humidity is not None else None,
        "stage": predicted_stage,
        "stage_confidence": float(round(stage_conf, 3)),
        "required_mm_per_m2": float(round(required_mm_per_m2, 3)),
        "expected_mm_per_m2": float(round(expected_mm_per_m2, 3)),
        "total_water_liters": float(round(expected_mm_per_m2 * farm_size_acres * FARM_M2_PER_ACRE, 2)),
        "evapouration_rate_pp_per_day": float(round(EVAP_PP_PER_DAY, 6)),
        "predicted_irrigation_time_iso": predicted_time_iso,
        "predicted_time_to_threshold_hours": predicted_time_to_threshold_hours,
        "observations": observations,
        "evap_updated_on_this_trigger": bool(evap_updated)
    }
    try:
        PRED_LATEST_REF.set(payload)
        print(f"[info] prediction updated at /farms/{FARM_ID}/predictions/latest (trigger={trigger_source})")
    except Exception as e:
        print("[error] failed to write predictions/latest:", e)

    return payload

# ----------------- Firestore realtime listeners ---------------------------
def _fire_snapshot_to_trigger(name, doc_snapshot, changes, read_time):
    # small helper to log what changed and call recompute
    try:
        changed_fields = None
        # changes is a list of DocumentChange for collection listeners; for doc listeners it's sometimes empty
        if changes:
            # attempt to list field masks from changes if available
            try:
                changed_fields = [c.document._data for c in changes]  # best-effort; internal
            except Exception:
                changed_fields = None
        print(f"[listener] {name} change detected. re-computing predictions...")
    except Exception:
        pass
    try:
        recompute_and_publish(trigger_source=name)
    except Exception as e:
        print(f"[listener {name}] recompute error:", e)

def soil_listener_callback(doc_snapshot, changes, read_time):
    _fire_snapshot_to_trigger("soil_change", doc_snapshot, changes, read_time)

def temp_listener_callback(doc_snapshot, changes, read_time):
    _fire_snapshot_to_trigger("temp_change", doc_snapshot, changes, read_time)

def humidity_listener_callback(doc_snapshot, changes, read_time):
    _fire_snapshot_to_trigger("humidity_change", doc_snapshot, changes, read_time)

def farm_doc_listener(doc_snapshot, changes, read_time):
    # doc_snapshot is a list (single DocumentSnapshot). We log and trigger recompute.
    try:
        # Try to show which top-level fields changed (best-effort)
        if doc_snapshot and len(doc_snapshot) > 0:
            ds = doc_snapshot[0]
            data = ds.to_dict() if ds.exists else {}
            print("[listener] farm doc updated. top-level keys now:", list(data.keys()))
    except Exception:
        pass
    _fire_snapshot_to_trigger("farm_doc_change", doc_snapshot, changes, read_time)

def reading_doc_listener(doc_snapshot, changes, read_time):
    try:
        if doc_snapshot and len(doc_snapshot) > 0:
            ds = doc_snapshot[0]
            data = ds.to_dict() if ds.exists else {}
            print("[listener] today's reading doc updated. top-level keys now:", list(data.keys()))
    except Exception:
        pass
    _fire_snapshot_to_trigger("reading_doc_change", doc_snapshot, changes, read_time)

def attach_listeners():
    date_key = today_date_key_harare()
    listeners = []
    try:
        soil_ref = sensor_doc_ref(FARM_ID, date_key, "soil")
        soil_ref.on_snapshot(soil_listener_callback)
        listeners.append(("soil", soil_ref.path))
    except Exception as e:
        print("[warn] soil listener failed:", e)
    try:
        temp_ref = sensor_doc_ref(FARM_ID, date_key, "temperature")
        temp_ref.on_snapshot(temp_listener_callback)
        listeners.append(("temperature", temp_ref.path))
    except Exception as e:
        print("[warn] temp listener failed:", e)
    try:
        hum_ref = sensor_doc_ref(FARM_ID, date_key, "humidity")
        hum_ref.on_snapshot(humidity_listener_callback)
        listeners.append(("humidity", hum_ref.path))
    except Exception as e:
        print("[warn] humidity listener failed:", e)

    # NEW: listen to farm doc for metadata changes (crop, initial_height, plant_date, size, ph, current_height, etc.)
    try:
        FARM_DOC_REF.on_snapshot(farm_doc_listener)
        listeners.append(("farm_doc", FARM_DOC_REF.path))
    except Exception as e:
        print("[warn] farm doc listener failed:", e)

    # NEW: listen to today's reading doc for top-level changes (avg_humidity, sensors map, etc.)
    try:
        reading_ref = reading_doc_ref(FARM_ID, date_key)
        reading_ref.on_snapshot(reading_doc_listener)
        listeners.append(("reading_doc", reading_ref.path))
    except Exception as e:
        print("[warn] reading doc listener failed:", e)

    print("[info] listeners attached to:", listeners)

# ------------------------------ MAIN --------------------------------------
if __name__ == "__main__":
    print("Starting irrigation_firestore_ml_final.py (with farm+reading listeners)")
    print("Using Harare timezone. Today's date key:", today_date_key_harare())
    load_state()
    try:
        payload = recompute_and_publish(trigger_source="startup_initial")
        print("[startup] prediction payload:", payload)
    except Exception as e:
        print("[startup] initial recompute failed:", e)
        raise
    attach_listeners()
    print("[info] Running; listening for sensor/farm/reading changes. Press Ctrl+C to exit.")
    try:
        while True:
            time.sleep(60)
    except KeyboardInterrupt:
        print("Stopping and exiting.")
