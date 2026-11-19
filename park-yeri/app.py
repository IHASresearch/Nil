from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import Lock
from uuid import uuid4

import cv2
from flask import Flask, jsonify, render_template, request, send_from_directory
from werkzeug.utils import secure_filename
from ultralytics import YOLO

from main import IMAGE_EXTENSIONS, VIDEO_EXTENSIONS, process_image, process_video


BASE_DIR = Path(__file__).resolve().parent
MODEL_PATH = BASE_DIR / "best.pt"
UPLOAD_DIR = BASE_DIR / "yuklenenler"
RESULT_DIR = BASE_DIR / "sonuclar"
ALLOWED_EXTENSIONS = IMAGE_EXTENSIONS | VIDEO_EXTENSIONS

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 500 * 1024 * 1024

executor = ThreadPoolExecutor(max_workers=1)
model_lock = Lock()
model = None
jobs = {}
jobs_lock = Lock()


def get_model():
    global model
    with model_lock:
        if model is None:
            if not MODEL_PATH.is_file():
                raise FileNotFoundError(f"Model dosyası bulunamadı: {MODEL_PATH}")
            model = YOLO(str(MODEL_PATH))
        return model


def process_job(job_id, paths, imgsz, conf):
    job_dir = RESULT_DIR / job_id
    job_dir.mkdir(parents=True, exist_ok=True)
    with jobs_lock:
        jobs[job_id]["status"] = "processing"

    try:
        detector = get_model()
        for index, input_path in enumerate(paths):
            suffix = input_path.suffix.lower()
            is_image = suffix in IMAGE_EXTENSIONS
            output_name = input_path.name if is_image else f"{input_path.stem}_sonuc.mp4"
            output_path = job_dir / output_name
            try:
                if is_image:
                    process_image(detector, input_path, output_path, imgsz=imgsz, conf=conf)
                else:
                    process_video(detector, input_path, output_path, imgsz=imgsz, conf=conf)
                if output_path.is_file() and output_path.stat().st_size > 0:
                    result = {
                        "name": input_path.name,
                        "url": f"/results/{job_id}/{output_name}",
                        "type": "image" if is_image else "video",
                    }
                    with jobs_lock:
                        jobs[job_id]["results"].append(result)
                else:
                    raise RuntimeError("Sonuç dosyası oluşturulamadı.")
            except Exception as exc:
                with jobs_lock:
                    jobs[job_id]["errors"].append({"name": input_path.name, "message": str(exc)})

            with jobs_lock:
                jobs[job_id]["completed"] = index + 1
                jobs[job_id]["progress"] = round((index + 1) * 100 / len(paths))

        with jobs_lock:
            jobs[job_id]["status"] = "done"
    except Exception as exc:
        with jobs_lock:
            jobs[job_id]["status"] = "error"
            jobs[job_id]["message"] = str(exc)


@app.get("/")
def index():
    return render_template("index.html")


@app.post("/api/jobs")
def create_job():
    uploads = request.files.getlist("files")
    if not uploads or all(not item.filename for item in uploads):
        return jsonify(error="Lütfen en az bir görsel veya video seçin."), 400

    try:
        imgsz = int(request.form.get("imgsz", "800"))
        conf = float(request.form.get("conf", "0.35"))
    except ValueError:
        return jsonify(error="Görüntü boyutu veya güven eşiği geçersiz."), 400
    if imgsz not in {640, 800, 1024, 1280} or not 0.05 <= conf <= 0.95:
        return jsonify(error="Ayarlar izin verilen aralığın dışında."), 400

    job_id = uuid4().hex
    upload_dir = UPLOAD_DIR / job_id
    upload_dir.mkdir(parents=True, exist_ok=True)
    saved_paths = []

    for item in uploads:
        if not item.filename:
            continue
        safe_name = secure_filename(item.filename)
        suffix = Path(safe_name).suffix.lower()
        if not safe_name or suffix not in ALLOWED_EXTENSIONS:
            continue
        stored_path = upload_dir / f"{len(saved_paths) + 1:03d}_{safe_name}"
        item.save(stored_path)
        saved_paths.append(stored_path)

    if not saved_paths:
        return jsonify(error="Desteklenen bir fotoğraf veya video bulunamadı."), 400

    with jobs_lock:
        jobs[job_id] = {
            "status": "queued", "progress": 0, "completed": 0,
            "total": len(saved_paths), "results": [], "errors": [],
        }
    executor.submit(process_job, job_id, saved_paths, imgsz, conf)
    return jsonify(job_id=job_id), 202


@app.get("/api/jobs/<job_id>")
def get_job(job_id):
    with jobs_lock:
        job = jobs.get(job_id)
        if job is None:
            return jsonify(error="İş bulunamadı."), 404
        return jsonify(job)


@app.get("/results/<job_id>/<path:filename>")
def result_file(job_id, filename):
    if len(job_id) != 32 or any(char not in "0123456789abcdef" for char in job_id):
        return "Bulunamadı", 404
    return send_from_directory(RESULT_DIR / job_id, filename, as_attachment=False)


@app.errorhandler(413)
def file_too_large(_error):
    return jsonify(error="Toplam dosya boyutu 500 MB sınırını aşıyor."), 413


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5000, debug=False)
