from pathlib import Path
import argparse

import cv2
from ultralytics import YOLO


# Desteklenen dosya biçimleri
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".webp", ".tif", ".tiff"}
VIDEO_EXTENSIONS = {".mp4", ".avi", ".mov", ".mkv", ".wmv", ".mpeg", ".mpg", ".m4v"}

# Modelde bu isimlerle karşılaşılırsa aşağıdaki gruplara eşlenir.
EMPTY_LABELS = {"empty", "space-empty", "free"}
OCCUPIED_LABELS = {"occupied", "space-occupied", "car"}


def normalize_label(label):
    """Etiketleri büyük/küçük harf ve ayraç farklarından bağımsız karşılaştırır."""
    return str(label).strip().lower().replace("_", "-").replace(" ", "-")


def label_group(label):
    label = normalize_label(label)
    if label in EMPTY_LABELS:
        return "empty"
    if label in OCCUPIED_LABELS:
        return "occupied"
    return None


def get_class_name(names, class_id):
    if isinstance(names, dict):
        return names.get(class_id, names.get(str(class_id), str(class_id)))
    if 0 <= class_id < len(names):
        return names[class_id]
    return str(class_id)


def tile_positions(length, tile_size, overlap):
    """Görüntünün kenarlarını da kapsayacak biçimde örtüşen döşeme başlangıçları üretir."""
    if length <= tile_size:
        return [0]
    stride = max(1, int(tile_size * (1.0 - overlap)))
    positions = list(range(0, max(1, length - tile_size + 1), stride))
    last = length - tile_size
    if positions[-1] != last:
        positions.append(last)
    return positions


def detect_frame(model, frame, imgsz=800, conf=0.35, overlap=0.20):
    """Kareyi tarar, farklı sınıflar arasındaki çakışan kutuları temizler."""
    height, width = frame.shape[:2]
    # Görsel çok büyük değilse bölmeden tek seferde tara; çok büyükse parçalara böl
    tile_size = max(width, height) if max(width, height) <= 1920 else imgsz
    xs = tile_positions(width, tile_size, overlap)
    ys = tile_positions(height, tile_size, overlap)
    detections = []

    for y in ys:
        for x in xs:
            crop = frame[y:min(y + tile_size, height), x:min(x + tile_size, width)]
            result = model.predict(
                source=crop,
                imgsz=imgsz,
                conf=conf,
                iou=0.45,
                agnostic_nms=True,
                verbose=False,
            )[0]
            if result.boxes is None:
                continue
            for box in result.boxes:
                class_id = int(box.cls[0].item())
                group = label_group(get_class_name(model.names, class_id))
                if group is None:
                    continue
                x1, y1, x2, y2 = map(float, box.xyxy[0].tolist())
                confidence = float(box.conf[0].item())
                detections.append((x1 + x, y1 + y, x2 + x, y2 + y,
                                   class_id, confidence, group))

    if not detections:
        return []
    boxes = [[int(d[0]), int(d[1]), max(1, int(d[2] - d[0])),
              max(1, int(d[3] - d[1]))] for d in detections]
    scores = [d[5] for d in detections]
    keep = cv2.dnn.NMSBoxes(boxes, scores, score_threshold=0.0, nms_threshold=0.40)
    return [detections[int(index)] for index in keep]


def draw_result(frame, detections):
    """Algılanan boş/dolu yerleri çizer ve karedeki anlık sayıları döndürür."""
    empty_count = 0
    occupied_count = 0

    for x1, y1, x2, y2, class_id, confidence, group in detections:
        x1, y1, x2, y2 = map(int, (x1, y1, x2, y2))
        if group == "empty":
            empty_count += 1
            color = (0, 200, 0)  # BGR yeşil
            caption = f"EMPTY {confidence:.2f}"
        else:
            occupied_count += 1
            color = (0, 0, 230)  # BGR kırmızı
            caption = f"OCCUPIED {confidence:.2f}"

        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
        text_y = max(22, y1 - 7)
        cv2.putText(frame, caption, (x1, text_y), cv2.FONT_HERSHEY_SIMPLEX,
                    0.55, color, 2, cv2.LINE_AA)

    total_count = empty_count + occupied_count
    panel_text = (
        f"Bos Park Yeri: {empty_count} | Dolu: {occupied_count} | Toplam: {total_count}"
    )
    font = cv2.FONT_HERSHEY_SIMPLEX
    font_scale = 0.75
    thickness = 2
    (text_width, text_height), baseline = cv2.getTextSize(panel_text, font, font_scale, thickness)
    pad = 12
    panel_width = min(frame.shape[1], text_width + pad * 2)
    panel_height = text_height + baseline + pad * 2
    overlay = frame.copy()
    cv2.rectangle(overlay, (0, 0), (panel_width, panel_height), (20, 20, 20), -1)
    cv2.addWeighted(overlay, 0.78, frame, 0.22, 0, frame)
    cv2.putText(frame, panel_text, (pad, pad + text_height), font, font_scale,
                (255, 255, 255), thickness, cv2.LINE_AA)
    return frame


def process_image(model, input_path, output_path, imgsz, conf):
    image = cv2.imread(str(input_path))
    if image is None:
        print(f"[Atlandı] Görüntü okunamadı: {input_path.name}")
        return

    detections = detect_frame(model, image, imgsz=imgsz, conf=conf)
    image = draw_result(image, detections)
    if not cv2.imwrite(str(output_path), image):
        print(f"[Hata] Görüntü kaydedilemedi: {output_path}")
        return
    print(f"[Tamamlandı] {input_path.name} -> {output_path}")


def process_video(model, input_path, output_path, imgsz, conf):
    capture = cv2.VideoCapture(str(input_path), cv2.CAP_FFMPEG)
    if not capture.isOpened():
        capture.release()
        capture = cv2.VideoCapture(str(input_path))
    if not capture.isOpened():
        print(f"[Atlandı] Video açılamadı: {input_path.name}")
        return

    width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = capture.get(cv2.CAP_PROP_FPS)
    if fps <= 0:
        fps = 25.0

    writer = cv2.VideoWriter(
        str(output_path), cv2.VideoWriter_fourcc(*"mp4v"), fps, (width, height)
    )
    if not writer.isOpened():
        capture.release()
        print(f"[Hata] Çıktı videosu oluşturulamadı: {output_path}")
        return

    frame_number = 0
    try:
        while True:
            ok, frame = capture.read()
            if not ok:
                break
            detections = detect_frame(model, frame, imgsz=imgsz, conf=conf)
            annotated = draw_result(frame, detections)
            writer.write(annotated)
            frame_number += 1
            if frame_number % 100 == 0:
                print(f"  {input_path.name}: {frame_number} kare işlendi")
    finally:
        capture.release()
        writer.release()
    print(f"[Tamamlandı] {input_path.name} ({frame_number} kare) -> {output_path}")


def main():
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="YOLO ile otopark görüntü ve videolarında boş/dolu yer tespiti"
    )
    parser.add_argument(
        "--input", default=str(script_dir / "test_gorselleri"),
        help="İşlenecek fotoğraf/video klasörü (varsayılan: test_gorselleri/)",
    )
    parser.add_argument(
        "--output", default=str(script_dir / "sonuclar"),
        help="Sonuçların kaydedileceği klasör (varsayılan: sonuclar/)",
    )
    parser.add_argument(
        "--imgsz", type=int, choices=(640, 800, 1024, 1280), default=800,
        help="Model giriş boyutu (varsayılan: 800)",
    )
    parser.add_argument(
        "--conf", type=float, default=0.35,
        help="Minimum güven eşiği (varsayılan: 0.35)",
    )
    args = parser.parse_args()

    model_path = script_dir / "best.pt"
    input_dir = Path(args.input).expanduser().resolve()
    output_dir = Path(args.output).expanduser().resolve()

    if not model_path.is_file():
        raise FileNotFoundError(f"Model dosyası bulunamadı: {model_path}")
    if not input_dir.is_dir():
        raise FileNotFoundError(
            f"Girdi klasörü bulunamadı: {input_dir}\n"
            "Klasörü oluşturun veya --input ile başka bir klasör belirtin."
        )

    files = sorted(
        (path for path in input_dir.iterdir()
         if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS | VIDEO_EXTENSIONS),
        key=lambda path: path.name.casefold(),
    )
    if not files:
        print(f"İşlenecek fotoğraf veya video bulunamadı: {input_dir}")
        return

    output_dir.mkdir(parents=True, exist_ok=True)
    model = YOLO(str(model_path))
    print(f"Model: {model_path.name} | Sınıflar: {model.names}\nGirdi: {input_dir}\nÇıktı: {output_dir}\n")

    for path in files:
        if path.suffix.lower() in IMAGE_EXTENSIONS:
            output_path = output_dir / path.name
            process_image(model, path, output_path, imgsz=args.imgsz, conf=args.conf)
        else:
            output_path = output_dir / f"{path.stem}_sonuc.mp4"
            process_video(model, path, output_path, imgsz=args.imgsz, conf=args.conf)

    print("\nTüm dosyalar işlendi.")


if __name__ == "__main__":
    main()
    