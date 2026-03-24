FROM stateoftheartio/qt6:6.6-mingw-aqt

USER root
RUN apt-get update && apt-get install -y zip && rm -rf /var/lib/apt/lists/*

USER user
WORKDIR /home/user/project

CMD sh -lc '\
  qt-cmake . -G Ninja -B build-win && \
  cmake --build build-win -j"$(nproc)" && \
  mkdir -p build-win/deploy && \
  windeployqt --dir build-win/deploy --libdir build-win/deploy/libs --plugindir build-win/deploy/plugins build-win/*.exe && \
  cp build-win/*.exe build-win/deploy/ && \
  cd build-win && zip -r app-win64.zip deploy \
'
