import sys


def read_metrics(file_name):
    metrics = {}
    with open(file_name, encoding="utf-8") as input_file:
        for line in input_file:
            fields = line.split()
            if len(fields) != 2:
                continue
            try:
                metrics[fields[0]] = int(fields[1])
            except ValueError:
                pass
    return metrics


def require_equal(metrics, left, right_names):
    names = [left] + right_names
    missing = [name for name in names if name not in metrics]
    if missing:
        raise RuntimeError("missing metrics: " + ", ".join(missing))
    expected = sum(metrics[name] for name in right_names)
    if metrics[left] != expected:
        raise RuntimeError(left + " is " + str(metrics[left]) + " but expected " + str(expected))


def main():
    if len(sys.argv) != 2:
        raise RuntimeError("usage: python3 tests/check_branch_metrics.py statistics.txt")
    metrics = read_metrics(sys.argv[1])
    require_equal(metrics, "branch.branches_resolved", ["branch.correct_predictions", "branch.direction_mispredictions", "branch.target_mispredictions"])
    require_equal(metrics, "branch.branches_resolved", ["branch.conditional_branches", "branch.unconditional_branches"])
    require_equal(metrics, "branch.branches_resolved", ["branch.taken_branches", "branch.not_taken_branches"])
    require_equal(metrics, "branch.redirects", ["branch.direction_mispredictions", "branch.target_mispredictions", "branch.false_branch_redirects"])
    for predictor_number in range(3):
        prefix = "predictor" + str(predictor_number) + "."
        require_equal(metrics, prefix + "lookups", [prefix + "branches_found", prefix + "branches_not_found"])
        require_equal(metrics, prefix + "updates", [prefix + "correct_predictions", prefix + "direction_mispredictions", prefix + "target_mispredictions"])
        if metrics[prefix + "updates"] != metrics["branch.branches_resolved"]:
            raise RuntimeError(prefix + "updates does not match resolved branches")
    print("BRANCH METRICS OK")


if __name__ == "__main__":
    main()
