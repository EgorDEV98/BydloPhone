# BydloPhone STT

HTTP-сервис для распознавания WAV-файлов через FastAPI и faster-whisper.

## Сборка

```bash
docker build -t bydlophone-stt .
```

## Запуск

```bash
docker run --rm -p 8000:8000 bydlophone-stt
```

## Проверка

```bash
curl -X POST http://localhost:8000/transcribe \
  -F "file=@voice.wav"
```

Ответ:

```json
{
  "text": "распознанный текст"
}
```
