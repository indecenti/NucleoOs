import importlib.util as u
print("wordfreq:", bool(u.find_spec("wordfreq")))
try:
    from wordfreq import top_n_list
    it = top_n_list("it", 20); en = top_n_list("en", 20)
    print("IT top20:", it)
    print("EN top20:", en)
    print("IT available large:", len(top_n_list("it", 30000)))
    print("EN available large:", len(top_n_list("en", 30000)))
except Exception as e:
    print("wordfreq err:", e)
