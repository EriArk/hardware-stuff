FROM docker.io/library/python:3.12.13-slim-bookworm@sha256:4766d8b510c428e595d74b9cc5bbb2fae8e26316fffb4adc89908d79aacd58a2
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 PYTHONPATH=/app
WORKDIR /app
RUN mkdir -p /data /run/speech && chown -R 1000:1000 /data /run/speech
COPY src/octofox_library /app/octofox_library
USER 1000:1000
EXPOSE 8080
CMD ["python", "-m", "octofox_library.books_web"]
