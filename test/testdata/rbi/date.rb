# typed: strict

require "date"

T.assert_type!(Date.today, Date)
T.assert_type!(Date.today + 1, Date)

T.reveal_type(Date.now - Date.now) # error: Revealed type: `Rational`
T.reveal_type(Date.now - 10) # error: Revealed type: `Date`

T.reveal_type(DateTime.now - DateTime.now) # error: Revealed type: `Rational`
T.reveal_type(DateTime.now - 10) # error: Revealed type: `DateTime`
