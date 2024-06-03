sudo apt update
sudo apt install -y ninja-build ccache valgrind

cmake -GNinja -B /workspaces/skybison/build \
        -DCMAKE_TOOLCHAIN_FILE=util/linux.cmake \
        -DCMAKE_BUILD_TYPE=DebugOpt \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

cd /workspaces/skybison/build && ninja -j4

mkdir -p /var/tmp/django_minimal
pushd /var/tmp/django_minimal
/workspaces/skybison/benchmarks/benchmarks/django/setup_django_minimal.sh
popd
cd /workspaces/skybison && ./benchmarks/quickbench/diffrevs.py --run-django HEAD^ HEAD | tee django-benchmark-results.json
