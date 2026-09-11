FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends g++ libc6-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
# Structure SDK intacte : lib/*.so, lib/*.so.1.1, lib/HCNetSDKCom/ (chargé par nom)
COPY libs/ /app/lib/
COPY HCNetSDK.h test_sdk.cpp /app/

RUN g++ -o test_sdk test_sdk.cpp -I. -L/app/lib -lhcnetsdk -Wl,-rpath,/app/lib

ENV LD_LIBRARY_PATH=/app/lib
CMD ["./test_sdk"]
