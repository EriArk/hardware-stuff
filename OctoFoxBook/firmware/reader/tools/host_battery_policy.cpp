#include "battery_policy.h"
#include <cassert>
int main() {
    using A=BatteryPolicy::Action;
    BatteryPolicy p;
    assert(p.sample(3700,25)==A::None);
    assert(p.sample(3634,15)==A::Warn);
    assert(p.sample(3634,15)==A::None);
    assert(p.sample(3590,9)==A::None);
    assert(p.sample(0,0)==A::None);
    assert(p.sample(3567,8)==A::None);
    assert(p.sample(3590,9)==A::None); // A brief load sag is not shutdown.
    assert(p.sample(3567,8)==A::None);
    assert(p.sample(3567,8)==A::None);
    assert(p.sample(3567,8)==A::Protect);
    BatteryPolicy recovered;
    assert(recovered.sample(3600,10)==A::Warn);
    assert(recovered.sample(3640,16)==A::None);
    assert(recovered.sample(3600,10)==A::None); // No warning storm.
    assert(recovered.sample(3700,25)==A::None);
    assert(recovered.sample(3600,10)==A::Warn);
    BatteryPolicy invalid;
    for(int i=0;i<10;++i) assert(invalid.sample(4800,0)==A::None);
    assert(invalid.sample(3350,1)==A::Warn);
    assert(invalid.sample(0,0)==A::None); // Invalid sample resets confirmation.
    assert(invalid.sample(3350,1)==A::None);
    assert(invalid.sample(3350,1)==A::None);
    assert(invalid.sample(3350,1)==A::Protect);
}
