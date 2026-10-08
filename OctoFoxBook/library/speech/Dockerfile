FROM docker.io/library/python:3.12.13-slim-bookworm@sha256:4766d8b510c428e595d74b9cc5bbb2fae8e26316fffb4adc89908d79aacd58a2
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 MKL_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg libgomp1 util-linux \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /models /run/speech && chown 1000:1000 /run/speech
COPY speech/requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt
COPY speech/torch-requirements.txt /app/torch-requirements.txt
RUN pip install --no-cache-dir --no-deps --index-url https://download.pytorch.org/whl/cpu -r /app/torch-requirements.txt
COPY speech/download.py speech/voices.json speech/download_silero.py /app/
RUN python download.py && python download_silero.py
COPY speech/server.py /app/server.py
COPY speech/healthcheck.py /app/healthcheck.py
USER 1000:1000
CMD ["flock", "--nonblock", "--no-fork", "/run/speech/worker.lock", "python", "/app/server.py"]
