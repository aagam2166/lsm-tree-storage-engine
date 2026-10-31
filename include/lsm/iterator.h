#ifndef LSM_INCLUDE_LSM_ITERATOR_H_
#define LSM_INCLUDE_LSM_ITERATOR_H_

#include <string_view>
#include "lsm/types.h"

namespace lsm {

class InternalIterator {
 public:
  virtual ~InternalIterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(const InternalKey& target) = 0;
  virtual void Next() = 0;
  virtual InternalKey key() const = 0;
  virtual std::string_view value() const = 0;
};

}  // namespace lsm

#endif  // LSM_INCLUDE_LSM_ITERATOR_H_
