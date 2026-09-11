FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends g++ libc6-dev tzdata \
    && ln -sf /usr/share/zoneinfo/America/Toronto /etc/localtime \
    && rm -rf /var/lib/apt/lists/*

ENV TZ=America/Toronto

WORKDIR /app
# Structure SDK intacte : lib/*.so, lib/*.so.1.1, lib/HCNetSDKCom/ (chargé par nom)
COPY libs/ /app/lib/
COPY HCNetSDK.h test_sdk.cpp sdk_service.cpp find_files.cpp check_record.cpp /app/

RUN g++ -O2 -o test_sdk    test_sdk.cpp    -I. -L/app/lib -lhcnetsdk -Wl,-rpath,/app/lib && \
    g++ -O2 -o sdk_service sdk_service.cpp -I. -L/app/lib -lhcnetsdk -lpthread -Wl,-rpath,/app/lib && \
    g++ -O2 -o find_files  find_files.cpp  -I. -L/app/lib -lhcnetsdk -Wl,-rpath,/app/lib && \
    g++ -O2 -o check_record check_record.cpp -I. -L/app/lib -lhcnetsdk -Wl,-rpath,/app/lib

RUN mkdir -p /clips

ENV LD_LIBRARY_PATH=/app/lib
EXPOSE 8090
CMD ["./sdk_service", "8090"]
