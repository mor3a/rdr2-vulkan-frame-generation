#pragma once
namespace fgvk {
// A missing runtime read is not an edit. Compare saved selection with the
// configuration used at initialization, then acknowledge explicit applies.
class PresetSelection {
  int saved_,baseline_;
public:
  explicit PresetSelection(int initial):saved_(initial),baseline_(initial){}
  int Saved()const{return saved_;}
  void Save(int value){saved_=value;}
  void Applied(int value){if(value>=0 && value<=3)baseline_=value;}
  bool Pending()const{return saved_!=baseline_;}
};
}
