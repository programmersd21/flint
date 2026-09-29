def make_pair():
    n = 0
    def bump():
        nonlocal n
        n += 1
        return n
    def read():
        return n
    return [bump, read]

pair = make_pair()
i = 0
while i < 2000000:
    pair[0]()
    i += 1
print(pair[1]())
