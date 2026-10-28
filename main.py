import io
import os
from pathlib import Path

from flask import Flask, jsonify, request
from openai import OpenAI

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY")

app = Flask(__name__)


def get_openai_client():
    if not OPENAI_API_KEY:
        return None
    return OpenAI(api_key=OPENAI_API_KEY)


@app.route("/")
def main():
    return jsonify({
        "status": "Server running",
        "audio_endpoint": "http://129.21.118.25:5000/process-audio",
    })


@app.route("/process-audio", methods=["POST"])
def process_audio_endpoint():
    client = get_openai_client()
    if not client:
        return jsonify({"error": "Missing OPENAI_API_KEY environment variable"}), 500

    # 1. Extract audio bytes from either multipart form-data or raw request body
    file_bytes = None
    filename = "audio.wav"
    content_type = "audio/wav"

    if "audio" in request.files:
        uploaded_file = request.files["audio"]
        if uploaded_file.filename:
            filename = uploaded_file.filename
        content_type = uploaded_file.content_type or content_type
        file_bytes = uploaded_file.read()
    else:
        # Fallback for devices sending raw binary audio stream
        raw_data = request.get_data()
        if raw_data:
            file_bytes = raw_data
            if request.content_type:
                content_type = request.content_type

    if not file_bytes:
        return jsonify({"error": "No audio received. Provide an 'audio' form field or raw audio body."}), 400

    # 2. Transcribe audio with Whisper
    try:
        audio_payload = (filename, file_bytes, content_type)
        transcript = client.audio.transcriptions.create(
            model="whisper-1",
            file=audio_payload,
            response_format="text",
            language="en",
        )
    except Exception as e:
        return jsonify({"error": f"Transcription error: {str(e)}"}), 500

    prompt = transcript.strip()
    if not prompt:
        return jsonify({"error": "No speech detected in audio"}), 400

    # 3. Request LLM response
    try:
        response = client.chat.completions.create(
            model="gpt-4o-mini",
            messages=[
                {
                    "role": "system",
                    "content": "You are a helpful and concise voice assistant.",
                },
                {
                    "role": "user",
                    "content": prompt,
                },
            ],
        )
        ai_response = response.choices[0].message.content.strip()
    except Exception as e:
        return jsonify({"error": f"Chat completion error: {str(e)}"}), 500

    # 4. Print both to the terminal
    print("=" * 40)
    print("PROMPT (TRANSCRIPT):")
    print(prompt)
    print("-" * 40)
    print("RESPONSE:")
    print(ai_response)
    print("=" * 40)

    # 5. Return JSON response to client
    return jsonify({
        "prompt": prompt,
        "response": ai_response,
    })


if __name__ == "__main__":
    print("=" * 50)
    print("Server running on http://129.21.118.25:5000")
    print("POST audio to: http://129.21.118.25:5000/process-audio")
    print("=" * 50)
    app.run(host="0.0.0.0", port=5000, debug=True)

