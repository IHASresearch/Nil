const input = document.querySelector("#fileInput");
const dropzone = document.querySelector("#dropzone");
const selectedFiles = document.querySelector("#selectedFiles");
const startButton = document.querySelector("#startButton");
const fileCount = document.querySelector("#fileCount");
const errorBox = document.querySelector("#errorBox");
const confInput = document.querySelector("#conf");
const resultsGrid = document.querySelector("#resultsGrid");
const progressCard = document.querySelector("#progressCard");
let files = [];
let pollingTimer;

const formatSize = bytes => bytes < 1024 * 1024
  ? `${Math.max(1, Math.round(bytes / 1024))} KB`
  : `${(bytes / (1024 * 1024)).toFixed(1)} MB`;

function showError(message) {
  errorBox.textContent = message;
  errorBox.classList.add("visible");
}

function renderFiles() {
  selectedFiles.replaceChildren();
  files.forEach((file, index) => {
    const row = document.createElement("div");
    row.className = "file-row";
    const type = file.type.startsWith("video/") ? "VID" : "IMG";
    row.innerHTML = `<span class="file-type">${type}</span><span class="file-info"><b></b><small>${formatSize(file.size)}</small></span><button class="remove-file" type="button" aria-label="Dosyayı kaldır">×</button>`;
    row.querySelector(".file-info b").textContent = file.name;
    row.querySelector("button").addEventListener("click", () => {
      files.splice(index, 1);
      renderFiles();
    });
    selectedFiles.append(row);
  });
  fileCount.textContent = `${files.length} dosya`;
  startButton.disabled = files.length === 0;
}

function addFiles(newFiles) {
  const valid = [...newFiles].filter(file => file.type.startsWith("image/") || file.type.startsWith("video/"));
  if (valid.length !== newFiles.length) showError("Yalnızca fotoğraf ve video dosyaları eklenebilir.");
  else errorBox.classList.remove("visible");
  files = [...files, ...valid];
  renderFiles();
}

input.addEventListener("change", event => {
  addFiles(event.target.files);
  input.value = "";
});
for (const eventName of ["dragenter", "dragover"]) {
  dropzone.addEventListener(eventName, event => {
    event.preventDefault();
    dropzone.classList.add("dragover");
  });
}
for (const eventName of ["dragleave", "drop"]) {
  dropzone.addEventListener(eventName, event => {
    event.preventDefault();
    dropzone.classList.remove("dragover");
  });
}
dropzone.addEventListener("drop", event => addFiles(event.dataTransfer.files));
confInput.addEventListener("input", () => {
  document.querySelector("#confValue").textContent = Number(confInput.value).toFixed(2);
});

function setProgress(percent, title, detail, finished = false) {
  progressCard.classList.add("visible");
  progressCard.classList.toggle("finished", finished);
  document.querySelector("#progressBar").style.width = `${percent}%`;
  document.querySelector("#progressPercent").textContent = `${percent}%`;
  document.querySelector("#progressTitle").textContent = title;
  document.querySelector("#progressText").textContent = detail;
}

function showResults(results, errors = []) {
  resultsGrid.replaceChildren();
  if (!results.length && !errors.length) {
    resultsGrid.innerHTML = '<div class="empty-state"><span class="empty-symbol">▧</span><b>Sonuç oluşturulamadı</b><span>Dosya biçimini veya video codec desteğini kontrol et.</span></div>';
    return;
  }
  results.forEach(result => {
    const card = document.createElement("article");
    card.className = "result-card";
    const media = result.type === "image"
      ? `<img class="result-media" src="${result.url}" alt="İşlenmiş otopark görüntüsü">`
      : `<video class="result-media" src="${result.url}" controls preload="metadata"></video>`;
    card.innerHTML = `${media}<div class="result-meta"><b></b><span>${result.type === "image" ? "Fotoğraf analizi" : "Video analizi"}</span></div><div class="result-actions"><a class="download-link" download href="${result.url}">↓ Sonucu indir</a></div>`;
    card.querySelector(".result-meta b").textContent = result.name;
    resultsGrid.append(card);
  });
  errors.forEach(error => {
    const card = document.createElement("article");
    card.className = "result-card error-result";
    card.innerHTML = '<div class="result-meta"><b></b><span></span></div>';
    card.querySelector("b").textContent = error.name;
    card.querySelector("span").textContent = `İşlenemedi: ${error.message}`;
    resultsGrid.append(card);
  });
}

async function pollJob(jobId) {
  try {
    const response = await fetch(`/api/jobs/${jobId}`);
    const job = await response.json();
    setProgress(job.progress, job.status === "done" ? "Analiz tamamlandı" : "Dosyalar analiz ediliyor", `${job.completed} / ${job.total} dosya işlendi`, job.status === "done" || job.status === "error");
    if (job.status === "done") {
      clearInterval(pollingTimer);
      startButton.disabled = files.length === 0;
      document.querySelector("#resultStatus").textContent = `${job.results.length} sonuç hazır`;
      showResults(job.results, job.errors);
      return;
    }
    if (job.status === "error") {
      clearInterval(pollingTimer);
      startButton.disabled = files.length === 0;
      showError(job.message || "Analiz sırasında bir sorun oluştu.");
      return;
    }
  } catch (_error) {
    clearInterval(pollingTimer);
    startButton.disabled = files.length === 0;
    showError("Sunucu bağlantısı kesildi. Sayfayı yenileyip tekrar deneyin.");
  }
}

startButton.addEventListener("click", async () => {
  if (!files.length) return;
  errorBox.classList.remove("visible");
  startButton.disabled = true;
  document.querySelector("#resultStatus").textContent = "İşleniyor…";
  setProgress(0, "Analiz hazırlanıyor", "Dosyalar modele gönderiliyor…");
  const form = new FormData();
  files.forEach(file => form.append("files", file));
  form.append("imgsz", document.querySelector("#imgsz").value);
  form.append("conf", confInput.value);
  try {
    const response = await fetch("/api/jobs", { method: "POST", body: form });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || "Yükleme başarısız oldu.");
    if (pollingTimer) clearInterval(pollingTimer);
    pollingTimer = setInterval(() => pollJob(data.job_id), 1200);
    await pollJob(data.job_id);
  } catch (error) {
    startButton.disabled = files.length === 0;
    showError(error.message);
  }
});
