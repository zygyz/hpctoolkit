#ifndef __MetricAccessorInband_hpp__
#define __MetricAccessorInband_hpp__

#include "MetricAccessor.hpp"


class MetricAccessorInband : public MetricAccessor {
public:
  MetricAccessorInband(Prof::Metric::IData &_mdata) : mdata(_mdata) {}
  ~MetricAccessorInband() {}
  double &idx(int mId, int size = 0) {
  double &idx(int mId, int size = 0) {
    return mdata.demandMetric(mId, size);
  }
  double c_idx(int mId) const {
    return mdata.metric(mId);
  }
private:
  Prof::Metric::IData &mdata;
};

#endif
