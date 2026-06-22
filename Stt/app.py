import os
import tempfile

from fastapi import FastAPI, File, UploadFile
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from faster_whisper import WhisperModel


app = FastAPI()

model = WhisperModel(
    "small",
    device="cpu",
    compute_type="int8",
)


@app.exception_handler(Exception)
async def handle_exception(request, exc):
    return JSONResponse(
        status_code=500,
        content={"error": str(exc)},
    )


@app.exception_handler(RequestValidationError)
async def handle_validation_exception(request, exc):
    return JSONResponse(
        status_code=500,
        content={"error": str(exc)},
    )


@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/transcribe")
async def transcribe(file: UploadFile | None = File(None)):
    temp_path = None

    try:
        if file is None:
            raise ValueError("File is required")

        content = await file.read()
        if not content:
            return {"text": ""}

        with tempfile.NamedTemporaryFile(delete=False, suffix=".wav") as temp_file:
            temp_file.write(content)
            temp_path = temp_file.name

        segments, _ = model.transcribe(temp_path, language="ru")
        text = " ".join(segment.text.strip() for segment in segments).strip()

        return {"text": text}
    except Exception as exc:
        return JSONResponse(
            status_code=500,
            content={"error": str(exc)},
        )
    finally:
        if temp_path and os.path.exists(temp_path):
            os.remove(temp_path)
        if file is not None:
            await file.close()
