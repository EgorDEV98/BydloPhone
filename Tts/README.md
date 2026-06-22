# BydloPhone TTS

HTTP-сервис для синтеза WAV-аудио через FastAPI и Coqui TTS XTTS v2.

Модель загружается один раз при старте приложения:

```python
TTS(model_name="tts_models/multilingual/multi-dataset/xtts_v2")
```

Поддерживаемые голосовые профили:

```text
bydlo
politic
teacher
robot
anime
```

Если передан неизвестный профиль, используется `bydlo`.

## Сборка

```bash
docker build -t bydlophone-tts .
```

## Запуск

```bash
docker run --rm -p 8001:8001 bydlophone-tts
```

## Пример запроса

```bash
curl -X POST http://localhost:8001/synthesize \
-H "Content-Type: application/json" \
-d '{
  "text":"Привет, мир",
  "voice":"bydlo"
}' \
--output answer.wav
```

Ответ возвращается как `audio/wav`, без Base64 и без JSON.
