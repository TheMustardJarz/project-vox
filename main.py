import os
from pathlib import Path

from flask import Flask
from openai import OpenAI

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY")

app = Flask(__name__)

@app.route("/")
def main():
    if not OPENAI_API_KEY:
        return "Missing OPENAI_API_KEY environment variable", 500

    client = OpenAI(api_key=OPENAI_API_KEY)
    audio_path = Path(__file__).with_name("TestRecording.mp3")

    if not audio_path.exists():
        return f"Audio file not found: {audio_path}", 404

    with audio_path.open("rb") as audio_file:
        transcript = client.audio.transcriptions.create(
            model="whisper-1",
            file=audio_file,
            response_format="text",
            language="en",
        )

    return transcript

if __name__ == "__main__":
    app.run(debug=True)
