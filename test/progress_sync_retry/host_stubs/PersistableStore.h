#pragma once

// Only SD persistence is replaced; tests exercise real CrossPointState JSON.
template <typename T>
class PersistableStore {
 public:
  static T& getInstance() {
    static T instance;
    return instance;
  }
};
