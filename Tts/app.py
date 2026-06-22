import os
import tempfile
import time
from pathlib import Path
from threading import Lock

from fastapi import FastAPI
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel
import torch
from TTS.api import TTS


app = FastAPI()

torch_threads = int(os.getenv("TORCH_NUM_THREADS", "4"))
torch.set_num_threads(torch_threads)
torch.set_num_interop_threads(1)

BASE_DIR = Path(__file__).resolve().parent

VOICE_MAP = {
    "bydlo": "voices/bydlo.wav",
    "politic": "voices/politic.wav",
    "teacher": "voices/teacher.wav",
    "robot": "voices/robot.wav",
    "anime": "voices/anime.wav",
}

def load_tts_model():
    attempts = int(os.getenv("TTS_MODEL_LOAD_ATTEMPTS", "5"))
    delay_seconds = int(os.getenv("TTS_MODEL_LOAD_RETRY_SECONDS", "15"))
    last_error = None

    for attempt in range(1, attempts + 1):
        try:
            return TTS(
                model_name="tts_models/multilingual/multi-dataset/xtts_v2",
            )
        except Exception as exc:
            last_error = exc
            if attempt == attempts:
                break

            print(
                f"Failed to load XTTS model on attempt {attempt}/{attempts}: {exc}. "
                f"Retrying in {delay_seconds} seconds.",
                flush=True,
            )
            time.sleep(delay_seconds)

    raise RuntimeError(f"Failed to load XTTS model: {last_error}")


tts = load_tts_model()
tts_lock = Lock()


class SynthesizeRequest(BaseModel):
    text: str
    voice: str = "bydlo"


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


@app.post("/synthesize")
def synthesize(payload: SynthesizeRequest):
    temp_path = None

    try:
        voice_path = BASE_DIR / VOICE_MAP.get(payload.voice, VOICE_MAP["bydlo"])
        print(
            f"Starting synthesis: text_length={len(payload.text)}, voice={payload.voice}, "
            f"torch_threads={torch_threads}",
            flush=True,
        )

        with tempfile.NamedTemporaryFile(delete=False, suffix=".wav") as temp_file:
            temp_path = temp_file.name

        with tts_lock:
            tts.tts_to_file(
                text=payload.text,
                speaker_wav=str(voice_path),
                language="ru",
                file_path=temp_path,
            )

        with open(temp_path, "rb") as wav_file:
            audio = wav_file.read()

        print(
            f"Finished synthesis: text_length={len(payload.text)}, bytes={len(audio)}",
            flush=True,
        )

        return Response(
            content=audio,
            media_type="audio/wav",
        )
    except Exception as exc:
        return JSONResponse(
            status_code=500,
            content={"error": str(exc)},
        )
    finally:
        if temp_path and os.path.exists(temp_path):
            os.remove(temp_path)
