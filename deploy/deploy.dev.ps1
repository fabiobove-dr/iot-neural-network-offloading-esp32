###################################################################################################################################################################################################
# Containers initialization
###################################################################################################################################################################################################
docker-compose -f ..\docker-compose.dev.yml --env-file ..\environment\.env.dev up -d --build
