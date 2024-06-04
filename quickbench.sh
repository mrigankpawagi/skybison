sudo apt update
sudo apt install -y ninja-build ccache valgrind

# sudo apt install libstdc++-12-dev libbz2-dev

cmake -GNinja -B build \
        -DCMAKE_TOOLCHAIN_FILE=util/linux.cmake \
        -DCMAKE_BUILD_TYPE=DebugOpt \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        # -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON

cd build && ninja -j4

mkdir -p /var/tmp/django_minimal
pushd /var/tmp/django_minimal
# $1 is the path to the skybison directory
$1/benchmarks/benchmarks/django/setup_django_minimal.sh
popd
cd $1 && ./benchmarks/benchmarks/django/benchmark.py --django-dir /var/tmp/django_minimal/ --interpreter ~/static-python/cinder/python --callgrind --callgrind-out-dir $(pwd) --num-requests 10
