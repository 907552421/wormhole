#include "../base/minibatch_iter.h"

int main(int argc, char *argv[]) {
  if (argc < 5) {
    printf("Usage: <libsvm> partid npart minibatch\n");
    return 0;
  }

  using namespace dmlc;
  data::MinibatchIter<unsigned> reader(
      argv[1], atoi(argv[2]), atoi(argv[3]), "libsvm", atoi(argv[4]));

  reader.BeforeFirst();
  size_t num_ex = 0;
  while (reader.Next()) {
    auto blk = reader.Value();
    num_ex += blk.size;
    LOG(INFO) << "minibatch " << blk.size << ", "
              << blk.offset[blk.size] << " index, "
              << num_ex << " examples";
  }
  return 0;
}
